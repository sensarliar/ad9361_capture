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

//#define CHECKSUM_ENABLE

/* helper macros */
#define MHZ(x) ((long long)(x*1000000.0 + .5))
#define GHZ(x) ((long long)(x*1000000000.0 + .5))

#define IIO_BUFFER_SIZE 128
#define IIO_BUFFER_BUS_WIDTHS 8

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
#ifdef CHECKSUM_ENABLE
#define CHECKSUM_CARRY(x) \
    (x = (x >> 16) + (x & 0xffff), (~(x + (x >> 16)) & 0xffff))

/**
 * code to do a ones-compliment checksum
 */
static int
do_checksum_math(uint16_t *data, int len)
{
    int sum = 0;
    union {
        uint16_t s;
        uint8_t b[2];
    } pad;

    while (len > 1) {
        sum += *data++;
        len -= 2;
    }

    if (len == 1) {
        pad.b[0] = *(uint8_t *)data;
        pad.b[1] = 0;
        sum += pad.s;
    }

    return (sum);
}
#endif
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
  
++(*id);
//  printf("id: %d\n", ++(*id));
u_char *buf;
    int sum;
    sum = 0;

 short len_left = pkthdr->len;
  unsigned int i=0;

unsigned int jjj=16;

	#ifdef CHECKSUM_ENABLE	
            sum = do_checksum_math((uint16_t *)packet, pkthdr->len);
            sum = CHECKSUM_CARRY(sum);	
	#endif
	
//	do{

		buf = iio_buffer_start(dds_buffer_gm);
/*
		buf[0]=0xAA;
		//buf[1]=(u_char)(*id);
buf[1]=0x55;
buf[2]=0xBB;
buf[3]=0x66;
buf[4]=0xCC;
buf[5]=0x77;
buf[6]=0xDD;
buf[7]=0x88;
*/
buf[0]=0x44;
buf[1]=0x55;
buf[2]=0x66;
buf[3]=0x77;
buf[4]=0x88;
buf[5]=0x99;
buf[6]=0x11;
buf[7]=0x22;

		buf[8]=(u_char)((pkthdr->len)&0xff);
		buf[9]=(u_char)(((pkthdr->len)&0xff00)>>8);
//buf[8] = 0x19;
//buf[9] = 0x91;

		buf[12]=(u_char)((*id)&0xff);
		buf[13]=(u_char)(((*id)&0xff00)>>8);

		if(len_left>IIO_BUFFER_BUS_WIDTHS*IIO_BUFFER_SIZE-16)
		{
		//buf[4]=0xf8;
		//buf[5]=0x03;	
		buf[10]=(u_char)((IIO_BUFFER_BUS_WIDTHS*IIO_BUFFER_SIZE-16)&0xff);
		buf[11]=(u_char)(((IIO_BUFFER_BUS_WIDTHS*IIO_BUFFER_SIZE-16)&0xff00)>>8);	
		}else
		{
		buf[10]=(u_char)(len_left&0xff);
		buf[11]=(u_char)((len_left&0xff00)>>8);		
		}
	#ifdef CHECKSUM_ENABLE	
            //sum = do_checksum_math((uint16_t *)(&buf[16]), IIO_BUFFER_BUS_WIDTHS*IIO_BUFFER_SIZE-16);
            //sum = CHECKSUM_CARRY(sum);
		buf[14]=(u_char)(sum&0xff);
		buf[15]=(u_char)((sum&0xff00)>>8);	
	#endif
		  for(; i<pkthdr->len; )
  			{
				buf[jjj] = packet[i];
//buf[jjj] = i;

				++i,++jjj;
				if(jjj>=IIO_BUFFER_BUS_WIDTHS*IIO_BUFFER_SIZE)
				{
		int ret = iio_buffer_push(dds_buffer_gm);
		if (ret < 0)
			printf("Error occured while writing to buffer: %d\n", ret);
				jjj=0;
				//break;
		buf = iio_buffer_start(dds_buffer_gm);
				}
			 }
if((jjj>0) &&(jjj<IIO_BUFFER_BUS_WIDTHS*IIO_BUFFER_SIZE))
{
				
iio_buffer_push(dds_buffer_gm);

}



	// len_left=len_left-(IIO_BUFFER_BUS_WIDTHS*IIO_BUFFER_SIZE-16);
	//}while(len_left>0);

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


/*
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
  

  pcap_t * device = pcap_open_live(devStr, 65535, 1, 0, errBuf);
  
  if(!device)
  {
    printf("error: pcap_open_live(): %s\n", errBuf);
    exit(1);
  }

device_eth1= device;

}
*/







int capture_function = 0;

bool stop_capture =0;
//static bool completed =0;
static int last_pk_id=0;
static u_char buff_send[65535];
static unsigned int buf_send_p=0;
static char sync_head[8];
static bool pkg_cont_flag =0;
unsigned int pk_total_num =0;


static bool capture_process(void)
{
static int lost_num =0;

    int sum_calc,sum_r;
    sum_calc = 0;
    sum_r = 0;
int mm;
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

			char *gm_p = iio_buffer_start(rxbuf);
			//u_char *gm_p = iio_buffer_start(rxbuf);
//printf("iio_buffer_start(rxbuf) %x: iio_buffer_end(rxbuf) %x,iio_buffer_end(rxbuf) %x\n",iio_buffer_start(rxbuf),iio_buffer_end(rxbuf),iio_buffer_step(rxbuf));


			int ii =0;
			int k=0;

RECAPTURE:
if(!pkg_cont_flag)
{
			for(;k<sample_count*IIO_BUFFER_BUS_WIDTHS-8;k++)
			{


//if(k<sample_count*IIO_BUFFER_BUS_WIDTHS)
//				printf("data count %x: value %x\n",k,*(gm_p));

				if(strncmp(gm_p,sync_head,8)==0)
				{
				printf("sync_head found:%d\n",k);
				break;
				}
				gm_p++;
			}
			if(k==sample_count*IIO_BUFFER_BUS_WIDTHS-8)
			{
			lost_num++;
			printf("sync_head lost:%d\n",lost_num);

			return 0;
			}
printf("head hex:%x,%x\n",*(gm_p+8),*(gm_p+9));

//		pk_total_num= *((short *)gm_p+5+4);
//pk_total_num= *((short *)gm_p+5+4);
			//	int this_pk_num=*((short *)gm_p+6);
			//	int packet_id= *((short *)gm_p+7);
			//	sum_r = *((short *)gm_p+8);

				printf("all num:%d\n",pk_total_num);
				//printf("this packet:%d\n",this_pk_num);
				//printf("packet id:%d\n",packet_id);
pk_total_num= *(gm_p+8)+((*(gm_p+9))<<8);

ii=k+16;
gm_p=gm_p+16;

}
else
{
}
		


				for(;(ii<sample_count*IIO_BUFFER_BUS_WIDTHS)&&(buf_send_p<pk_total_num);ii++)
				{
				//if((ii<6)||(ii>sample_count*2-3))
				//printf("data count %d: value %d\n",ii,*(gm_p));
		

					buff_send[buf_send_p]=*(gm_p);
					buf_send_p++;


				gm_p++;
				}
	printf("buf_send_p,%d\n",buf_send_p);

				if(buf_send_p==pk_total_num)
				{

	#ifdef CHECKSUM_ENABLE	
            sum_calc = do_checksum_math((uint16_t *)buff_send, pk_total_num);
            sum_calc = CHECKSUM_CARRY(sum_calc);	
	if(sum_r == sum_calc)
	{
	printf("checksum crc received wrong\n");
	buf_send_p=0;
	return !stop_capture;
	}
	#endif




buff_send[0]=0xff;
buff_send[1]=0xff;
buff_send[2]=0xff;
buff_send[3]=0xff;
buff_send[4]=0xff;
buff_send[5]=0xff;
/*
buff_send[6]=0x66;
buff_send[7]=0x0d;
buff_send[8]=0x06;
buff_send[9]=0xc8;
buff_send[10]=0x0a;
buff_send[11]=0xe3;


char gg;
gg=buff_send[29];
//buff_send[29]=buff_send[33];
buff_send[29]=0x58;
buff_send[33]=0x4d;

*/



				//printf("data buf_send_p %d,pk_total_num:%d\n",buf_send_p,pk_total_num);
				pcap_inject(device_eth0,buff_send,pk_total_num);
//int inject_num=pcap_inject(device_eth0,buff_send,pk_total_num);
				//printf("send out datanum: %d,id:%d\n",inject_num,packet_id);
				buf_send_p=0;
pkg_cont_flag =0;
if(ii<sample_count*IIO_BUFFER_BUS_WIDTHS)
{
k=ii;
goto RECAPTURE;
}
				}
				else if(buf_send_p>pk_total_num)
				{
				printf("something wrong\n");
				buf_send_p=0;
pkg_cont_flag =0;
				}
				else
				{
//				last_pk_id = packet_id;
pkg_cont_flag =1;
				}





	return !stop_capture;
}







/* simple configuration and streaming */
int main (int argc, char **argv)
{


sync_head[0]=0x44;
sync_head[1]=0x55;
sync_head[2]=0x66;
sync_head[3]=0x77;
sync_head[4]=0x88;
sync_head[5]=0x99;
sync_head[6]=0x11;
sync_head[7]=0x22;

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
	assert(cfg_ad9361_streaming_ch(ctx, &rxcfg, RX, 0) && "RX port 0 not found");
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


printf("iio_device_get_sample_size  %d\n",iio_device_get_sample_size(tx));
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
