/*
 * libiio - AD9361 IIO streaming example
 *
 * Copyright (C) 2014 IABG mbH
 * Author: Michael Feilen <feilen_at_iabg.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 **/

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <iio.h>

#include <pcap.h>
#include <time.h>
#include <stdlib.h>

#include <string.h>
#include <stdbool.h>
#include <malloc.h>
#include <errno.h>
#include <math.h>
#include <ctype.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/utsname.h>
#endif
#include <matio.h>

#include <math.h>



/* helper macros */
#define MHZ(x) ((long long)(x*1000000.0 + .5))
#define GHZ(x) ((long long)(x*1000000000.0 + .5))

#define IIO_BUFFER_SIZE 512

  pcap_t * device_eth0; 
pcap_t * device_eth1;
struct iio_buffer *dds_buffer_gm;


/* RX is input, TX is output */
enum iodev { RX, TX };

/* common RX and TX streaming params */
struct stream_cfg {
	long long bw_hz; // Analog banwidth in Hz
	long long fs_hz; // Baseband sample rate in Hz
	long long lo_hz; // Local oscillator frequency in Hz
	const char* rfport; // Port name
};

/* static scratch mem for strings */
static char tmpstr[64];

/* IIO structs required for streaming */
static struct iio_context *ctx   = NULL;
static struct iio_channel *rx0_i = NULL;
static struct iio_channel *rx0_q = NULL;
static struct iio_channel *tx0_i = NULL;
static struct iio_channel *tx0_q = NULL;
static struct iio_buffer  *rxbuf = NULL;
static struct iio_buffer  *txbuf = NULL;

static bool stop;

/* cleanup and exit */
static void shutdown()
{

 if (device_eth0){pcap_close(device_eth0);}
// if (device_eth1){pcap_close(device_eth1);}
	printf("* Destroying buffers\n");
	if (rxbuf) { iio_buffer_destroy(rxbuf); }
	if (txbuf) { iio_buffer_destroy(txbuf); }

	printf("* Disabling streaming channels\n");
	if (rx0_i) { iio_channel_disable(rx0_i); }
	if (rx0_q) { iio_channel_disable(rx0_q); }
	if (tx0_i) { iio_channel_disable(tx0_i); }
	if (tx0_q) { iio_channel_disable(tx0_q); }

	printf("* Destroying context\n");
	if (ctx) { iio_context_destroy(ctx); }
	exit(0);
}

static void handle_sig(int sig)
{
	printf("Waiting for process to finish...\n");
	stop = true;
}

/* check return value of attr_write function */
static void errchk(int v, const char* what) {
	 if (v < 0) { fprintf(stderr, "Error %d writing to channel \"%s\"\nvalue may not be supported.\n", v, what); shutdown(); }
}

/* write attribute: long long int */
static void wr_ch_lli(struct iio_channel *chn, const char* what, long long val)
{
	errchk(iio_channel_attr_write_longlong(chn, what, val), what);
}

/* write attribute: string */
static void wr_ch_str(struct iio_channel *chn, const char* what, const char* str)
{
	errchk(iio_channel_attr_write(chn, what, str), what);
}

/* helper function generating channel names */
static char* get_ch_name(const char* type, int id)
{
	snprintf(tmpstr, sizeof(tmpstr), "%s%d", type, id);
	return tmpstr;
}

/* returns ad9361 phy device */
static struct iio_device* get_ad9361_phy(struct iio_context *ctx)
{
	struct iio_device *dev =  iio_context_find_device(ctx, "ad9361-phy");
	assert(dev && "No ad9361-phy found");
	return dev;
}

/* finds AD9361 streaming IIO devices */
static bool get_ad9361_stream_dev(struct iio_context *ctx, enum iodev d, struct iio_device **dev)
{
	switch (d) {
	case TX: *dev = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc"); return *dev != NULL;
	case RX: *dev = iio_context_find_device(ctx, "cf-ad9361-lpc");  return *dev != NULL;
	default: assert(0); return false;
	}
}

/* finds AD9361 streaming IIO channels */
static bool get_ad9361_stream_ch(struct iio_context *ctx, enum iodev d, struct iio_device *dev, int chid, struct iio_channel **chn)
{
	*chn = iio_device_find_channel(dev, get_ch_name("voltage", chid), d == TX);
	if (!*chn)
		*chn = iio_device_find_channel(dev, get_ch_name("altvoltage", chid), d == TX);
	return *chn != NULL;
}

/* finds AD9361 phy IIO configuration channel with id chid */
static bool get_phy_chan(struct iio_context *ctx, enum iodev d, int chid, struct iio_channel **chn)
{
	switch (d) {
	case RX: *chn = iio_device_find_channel(get_ad9361_phy(ctx), get_ch_name("voltage", chid), false); return *chn != NULL;
	case TX: *chn = iio_device_find_channel(get_ad9361_phy(ctx), get_ch_name("voltage", chid), true);  return *chn != NULL;
	default: assert(0); return false;
	}
}

/* finds AD9361 local oscillator IIO configuration channels */
static bool get_lo_chan(struct iio_context *ctx, enum iodev d, struct iio_channel **chn)
{
	switch (d) {
	 // LO chan is always output, i.e. true
	case RX: *chn = iio_device_find_channel(get_ad9361_phy(ctx), get_ch_name("altvoltage", 0), true); return *chn != NULL;
	case TX: *chn = iio_device_find_channel(get_ad9361_phy(ctx), get_ch_name("altvoltage", 1), true); return *chn != NULL;
	default: assert(0); return false;
	}
}

/* applies streaming configuration through IIO */
bool cfg_ad9361_streaming_ch(struct iio_context *ctx, struct stream_cfg *cfg, enum iodev type, int chid)
{
	struct iio_channel *chn = NULL;

	// Configure phy and lo channels
	printf("* Acquiring AD9361 phy channel %d\n", chid);
	if (!get_phy_chan(ctx, type, chid, &chn)) {	return false; }
	wr_ch_str(chn, "rf_port_select",     cfg->rfport);
	wr_ch_lli(chn, "rf_bandwidth",       cfg->bw_hz);
	wr_ch_lli(chn, "sampling_frequency", cfg->fs_hz);

	// Configure LO channel
	printf("* Acquiring AD9361 %s lo channel\n", type == TX ? "TX" : "RX");
	if (!get_lo_chan(ctx, type, &chn)) { return false; }
	wr_ch_lli(chn, "frequency", cfg->lo_hz);
	return true;
}








void getPacket(u_char * arg, const struct pcap_pkthdr * pkthdr, const u_char * packet)
{
  short * id = (short *)arg;
  
//  printf("id: %d\n", ++(*id));
u_char *buf;


 short len_left = pkthdr->len;
  unsigned int i=0;

unsigned int jjj=14;
	
	do{

		buf = iio_buffer_start(dds_buffer_gm);
		buf[0]=0xAA;
		//buf[1]=(u_char)(*id);
buf[1]=0x55;
buf[2]=0xBB;
buf[3]=0x66;
buf[4]=0xCC;
buf[5]=0x77;
buf[6]=0xDD;
buf[7]=0x88;
		buf[8]=(u_char)((pkthdr->len)&0xff);
		buf[9]=(u_char)(((pkthdr->len)&0xff00)>>8);

		buf[12]=(u_char)((*id)&0xff);
		buf[13]=(u_char)(((*id)&0xff00)>>8);

		if(len_left>2*IIO_BUFFER_SIZE-14)
		{
		//buf[4]=0xf8;
		//buf[5]=0x03;	
		buf[10]=(u_char)((2*IIO_BUFFER_SIZE-8)&0xff);
		buf[11]=(u_char)(((2*IIO_BUFFER_SIZE-8)&0xff00)>>8);	
		}else
		{
		buf[10]=(u_char)(len_left&0xff);
		buf[11]=(u_char)((len_left&0xff00)>>8);		
		}
		  for(; i<pkthdr->len; )
  			{
				buf[jjj] = packet[i];
				++i,++jjj;
				if(jjj>=2*IIO_BUFFER_SIZE)
				{

				jjj=14;
				break;
				}
			 }

		int ret = iio_buffer_push(dds_buffer_gm);
		if (ret < 0)
			printf("Error occured while writing to buffer: %d\n", ret);


	 len_left=len_left-(2*IIO_BUFFER_SIZE-14);
	}while(len_left>0);

//usleep(300);


}


static void always_loop(void)
{

  int id = 0;

  pcap_loop(device_eth0, -1, getPacket, (u_char*)&id);
}

static void open_eth0(viod)
{

char errBuf[PCAP_ERRBUF_SIZE], * devStr;
  
devStr = "eth0";
  
  if(devStr)
  {
    printf("success: device: %s\n", devStr);
  }
  else
  {
    printf("error: %s\n", errBuf);
    exit(1);
  }
  
  /* open a device, wait until a packet arrives */
  pcap_t * device = pcap_open_live(devStr, 65535, 1, 0, errBuf);
  
  if(!device)
  {
    printf("error: pcap_open_live(): %s\n", errBuf);
    exit(1);
  }

device_eth0= device;

}



static void open_eth1(viod)
{

char errBuf[PCAP_ERRBUF_SIZE], * devStr;
  
devStr = "eth1";
  
  if(devStr)
  {
    printf("success: device: %s\n", devStr);
  }
  else
  {
    printf("error: %s\n", errBuf);
    exit(1);
  }
  
  /* open a device, wait until a packet arrives */
  pcap_t * device = pcap_open_live(devStr, 65535, 1, 0, errBuf);
  
  if(!device)
  {
    printf("error: pcap_open_live(): %s\n", errBuf);
    exit(1);
  }

device_eth1= device;

}







int capture_function = 0;

bool stop_capture =0;
//static bool completed =0;
static int last_pk_id=0;
static u_char buff_send[65535];
static unsigned int buf_send_p=0;



static bool capture_process(void)
{
static int lost_num =0;
//	unsigned int i;

			ssize_t ret = iio_buffer_refill(rxbuf);

			if (ret < 0) {
if(ret!=-110)
{
				fprintf(stderr, "Error while reading data: %s\n", strerror(-ret));
				//stop_sampling();

}
else
{
printf("iio_buffer_refill time out 1s");
return 0;
}
			}
int sample_count=IIO_BUFFER_SIZE;

	u_char *gm_p = iio_buffer_start(rxbuf);
				int ii =0;
				if(0xaa!=*gm_p)
				{
				lost_num++;
				printf("lost:%d\n",lost_num);
				return 0;
				}
				unsigned int pk_total_num= *((short *)gm_p+1);
				int this_pk_num=*((short *)gm_p+2);
				int packet_id= *((short *)gm_p+3);

				//printf("all num:%d\n",pk_total_num);
				//printf("this packet:%d\n",this_pk_num);
				//printf("packet id:%d\n",packet_id);

				if(buf_send_p>0)
				{
					if(last_pk_id!=packet_id)
					{
				buf_send_p=0;
				printf("packet not incomplete!\n");
				//break;
					}

				}

				for(;(ii<sample_count*2)&&(ii<this_pk_num+8);ii++)
				{
				//if((ii<6)||(ii>sample_count*2-3))
				//printf("data count %d: value %d\n",ii,*(gm_p));
		
					if(ii>7)
					{
					buff_send[buf_send_p]=*(gm_p);
					buf_send_p++;

					}

				gm_p++;
				}


				if(buf_send_p==pk_total_num)
				{

char exchange[6];

exchange[0]=buff_send[0];
exchange[1]=buff_send[1];
exchange[2]=buff_send[2];
exchange[3]=buff_send[3];
exchange[4]=buff_send[4];
exchange[5]=buff_send[5];


buff_send[0]=buff_send[6];
buff_send[1]=buff_send[7];
buff_send[2]=buff_send[8];
buff_send[3]=buff_send[9];
buff_send[4]=buff_send[10];
buff_send[5]=buff_send[11];

buff_send[6]=exchange[0];
buff_send[7]=exchange[1];
buff_send[8]=exchange[2];
buff_send[9]=exchange[3];
buff_send[10]=exchange[4];
buff_send[11]=exchange[5];

/*

buff_send[0]=0xff;
buff_send[1]=0xff;
buff_send[2]=0xff;
buff_send[3]=0xff;
buff_send[4]=0xff;
buff_send[5]=0xff;

buff_send[6]=0x66;
buff_send[7]=0x0d;
buff_send[8]=0x06;
buff_send[9]=0xc8;
buff_send[10]=0x0a;
buff_send[11]=0xe3;
*/
char gg;
gg=buff_send[29];
buff_send[29]=buff_send[33];
//buff_send[29]=0x11;
buff_send[33]=gg;


				//printf("data buf_send_p %d,pk_total_num:%d\n",buf_send_p,pk_total_num);
				int inject_num=pcap_inject(device_eth0,buff_send,pk_total_num);
				//printf("send out datanum: %d,id:%d\n",inject_num,packet_id);
				buf_send_p=0;
				}
				else if(buf_send_p>pk_total_num)
				{
				printf("something wrong\n");
				buf_send_p=0;
				}
				else
				{
				last_pk_id = packet_id;
				}





	return !stop_capture;
}







/* simple configuration and streaming */
int main (int argc, char **argv)
{
	// Streaming devices
	struct iio_device *tx;
	struct iio_device *rx;

	// RX and TX sample counters
	//size_t nrx = 0;
	//size_t ntx = 0;

	// Stream configurations
	struct stream_cfg rxcfg;
	struct stream_cfg txcfg;

	// Listen to ctrl+c and assert
	signal(SIGINT, handle_sig);

	// RX stream config
	rxcfg.bw_hz = MHZ(18);   // 2 MHz rf bandwidth
	rxcfg.fs_hz = MHZ(30.72);   // 2.5 MS/s rx sample rate
	rxcfg.lo_hz = GHZ(2.4);// 2.5 GHz rf frequency
	rxcfg.rfport = "A_BALANCED"; // port A (select for rf freq.)

	// TX stream config
	txcfg.bw_hz = MHZ(18); // 1.5 MHz rf bandwidth
	txcfg.fs_hz = MHZ(30.72);   // 2.5 MS/s tx sample rate
	txcfg.lo_hz = GHZ(2.4); // 2.5 GHz rf frequency
	txcfg.rfport = "A"; // port A (select for rf freq.)

	printf("* Acquiring IIO context\n");
	assert((ctx = iio_create_default_context()) && "No context");
	assert(iio_context_get_devices_count(ctx) > 0 && "No devices");

	printf("* Acquiring AD9361 streaming devices\n");
	assert(get_ad9361_stream_dev(ctx, TX, &tx) && "No tx dev found");
	assert(get_ad9361_stream_dev(ctx, RX, &rx) && "No rx dev found");

	printf("* Configuring AD9361 for streaming\n");
	assert(cfg_ad9361_streaming_ch(ctx, &rxcfg, RX, 1) && "RX port 0 not found");
	assert(cfg_ad9361_streaming_ch(ctx, &txcfg, TX, 0) && "TX port 0 not found");

	printf("* Initializing AD9361 IIO streaming channels\n");
	assert(get_ad9361_stream_ch(ctx, RX, rx, 0, &rx0_i) && "RX chan i not found");
	assert(get_ad9361_stream_ch(ctx, RX, rx, 1, &rx0_q) && "RX chan q not found");
	assert(get_ad9361_stream_ch(ctx, TX, tx, 0, &tx0_i) && "TX chan i not found");
	assert(get_ad9361_stream_ch(ctx, TX, tx, 1, &tx0_q) && "TX chan q not found");

	printf("* Enabling IIO streaming channels\n");
	iio_channel_enable(rx0_i);
	iio_channel_enable(rx0_q);
	iio_channel_enable(tx0_i);
	iio_channel_enable(tx0_q);

	printf("* Creating non-cyclic IIO buffers with 1 MiS\n");
		iio_device_set_kernel_buffers_count(rx,128);
	rxbuf = iio_device_create_buffer(rx, IIO_BUFFER_SIZE, false);
	if (!rxbuf) {
		perror("Could not create RX buffer");
		shutdown();
	}
	txbuf = iio_device_create_buffer(tx, IIO_BUFFER_SIZE, false);
	if (!txbuf) {
		perror("Could not create TX buffer");
		shutdown();
	}

open_eth0();
//open_eth1();


dds_buffer_gm =txbuf;

g_thread_new("pcap loop", (void *) &always_loop, NULL);

	printf("* Starting IO streaming (press CTRL+C to cancel)\n");

	while (!stop)
	{
	//	ssize_t nbytes_rx =0, nbytes_tx =0;

capture_process();


		// Sample counter increment and status output
	//	nrx += nbytes_rx / iio_device_get_sample_size(rx);
	//	ntx += nbytes_tx / iio_device_get_sample_size(tx);
	//	printf("\tRX %8.2f MSmp, TX %8.2f MSmp\n", nrx/1e6, ntx/1e6);
	}

	shutdown();

	return 0;
} 