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


   #include <pthread.h>
#include "timer.h"


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
#include <unistd.h>
#include "rxfifo_reset.h"

//#define CHECKSUM_ENABLE

/* helper macros */
#define MHZ(x) ((long long)(x*1000000.0 + .5))
#define GHZ(x) ((long long)(x*1000000000.0 + .5))

#define IIO_BUFFER_SIZE 16
#define IIO_BUFFER_BUS_WIDTHS 8

  pcap_t * device_eth0; 
pcap_t * device_eth1;
struct iio_buffer *dds_buffer_gm;

   pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/* RX is input, TX is output */
enum iodev { RX, TX };

/* common RX and TX streaming params */
struct stream_cfg {
	long long bw_hz; // Analog banwidth in Hz
	long long fs_hz; // Baseband sample rate in Hz
	long long lo_hz; // Local oscillator frequency in Hz
	const char* rfport; // Port name
	const char* gain_control_mode; //fast track
	long long hardwaregain; //hardwaregain 0db
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

	int uio_fd;
	void *uio_addr;

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
timer_stop();
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
	munmap(uio_addr, 0x1000);
	close(uio_fd);
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

if(type == TX)
{
	wr_ch_lli(chn, "hardwaregain", cfg->hardwaregain);
}else if(type == RX)
{
	wr_ch_str(chn, "gain_control_mode",     cfg->gain_control_mode);
}

	// Configure LO channel
	printf("* Acquiring AD9361 %s lo channel\n", type == TX ? "TX" : "RX");
	if (!get_lo_chan(ctx, type, &chn)) { return false; }
	wr_ch_lli(chn, "frequency", cfg->lo_hz);
	return true;
}


unsigned int rd_txfifo_hf_flag(void)
{
	unsigned int hf_flag=0;
//	uio_rd("/dev/mwipcore2", 0x00, &hf_flag);
//gm_rd("/dev/mwipcore2", &hf_flag);
//msync(uio_addr,100,MS_INVALIDATE);
lseek(uio_fd,0,SEEK_SET);
	hf_flag = *((unsigned *) (uio_addr));
	hf_flag = hf_flag&0x00000001;
	return hf_flag;

}



int flag_num = 0;

void getPacket(u_char * arg, const struct pcap_pkthdr * pkthdr, const u_char * packet)
{


     if(pthread_mutex_lock(&mutex)!=0)
     {                     
      perror("pthread_mutex_lock");                          
     }                                                      
     else  
{

  short * id = (short *)arg;
  
++(*id);
//  printf("id: %d\n", ++(*id));
u_char *buf;


 short len_left = pkthdr->len;
  unsigned int i=0;

unsigned int jjj=16;

	#ifdef CHECKSUM_ENABLE	
    int sum;
    sum = 0;
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
			#ifdef CHECKSUM_ENABLE	
			    //sum = do_checksum_math((uint16_t *)(&buf[16]), IIO_BUFFER_BUS_WIDTHS*IIO_BUFFER_SIZE-16);
			    //sum = CHECKSUM_CARRY(sum);
				buf[10]=(u_char)(sum&0xff);
				buf[11]=(u_char)((sum&0xff00)>>8);	
			#endif

				buf[12]=(u_char)((*id)&0xff);
				buf[13]=(u_char)(((*id)&0xff00)>>8);

				if(len_left>IIO_BUFFER_BUS_WIDTHS*IIO_BUFFER_SIZE-16)
				{
				//buf[4]=0xf8;
				//buf[5]=0x03;	
				buf[14]=(u_char)((IIO_BUFFER_BUS_WIDTHS*IIO_BUFFER_SIZE-16)&0xff);
				buf[15]=(u_char)(((IIO_BUFFER_BUS_WIDTHS*IIO_BUFFER_SIZE-16)&0xff00)>>8);	
				}else
				{
				buf[14]=(u_char)(len_left&0xff);
				buf[15]=(u_char)((len_left&0xff00)>>8);		
				}

				  for(; i<pkthdr->len; )
		  			{
						buf[jjj] = packet[i];
		//buf[jjj] = i;

						++i,++jjj;
						if(jjj>=IIO_BUFFER_BUS_WIDTHS*IIO_BUFFER_SIZE)
						{
		//printf("send big num :%d\n",jjj);
		int ret =0;

while(rd_txfifo_hf_flag())
{
flag_num++;
//printf("tx fifo half full flag was set %d.\n",flag_num);
	usleep(10);
}


				ret = iio_buffer_push(dds_buffer_gm);
				if (ret < 0)
					printf("Error occured while writing to buffer: %d\n", ret);
		  

						jjj=0;
						//break;
				buf = iio_buffer_start(dds_buffer_gm);
						}
					 }
		if((jjj>0) &&(jjj<IIO_BUFFER_BUS_WIDTHS*IIO_BUFFER_SIZE))
		{

		int tmp_jjj;
		tmp_jjj=jjj/IIO_BUFFER_BUS_WIDTHS;
		if(jjj%IIO_BUFFER_BUS_WIDTHS>0)
		tmp_jjj++;
		//printf("send num :%d\n",jjj);

		//iio_buffer_push_partial(dds_buffer_gm,tmp_jjj);			

		{
		memset((u_char *)(iio_buffer_start(dds_buffer_gm))+jjj,0,IIO_BUFFER_BUS_WIDTHS*IIO_BUFFER_SIZE-jjj);
		//iio_buffer_push_partial(dds_buffer_gm,tmp_jjj);

while(rd_txfifo_hf_flag())
{
flag_num++;
//printf("tx fifo half full flag was set %d.\n",flag_num);
	usleep(10);
}

		iio_buffer_push(dds_buffer_gm);
		}

		}

}
     if(pthread_mutex_unlock(&mutex)!=0){                   
     perror("pthread_mutex_unlock");                        
     }    
timer_set();
	// len_left=len_left-(IIO_BUFFER_BUS_WIDTHS*IIO_BUFFER_SIZE-16);
	//}while(len_left>0);

//usleep(300);


}

void tx_dirty_data(union sigval sigval)
{
int ret;
u_char *buf_temp;
ret=pthread_mutex_trylock(&mutex);                     
     if(ret==EBUSY)
{                                         
     //printf("pthread2:the variable is locked by pthread1\n");
}
     else
     {  
      if(ret!=0)
      {                                                                                      
        perror("pthread_mutex_trylock");                 
        exit(1);                                         
       }                                                
       else
{
buf_temp = iio_buffer_start(dds_buffer_gm);
memset(buf_temp,0,IIO_BUFFER_BUS_WIDTHS*IIO_BUFFER_SIZE);
iio_buffer_push(dds_buffer_gm);
}
                                             
       //printf("pthread2:pthread2 got lock.The variable is%d\n",lock_var);                                 
               /*互斥锁接锁*/                                   
       if(pthread_mutex_unlock(&mutex)!=0)
       {             
        perror("pthread_mutex_unlock");                  
       }                                                
       //else                                             
       //printf("pthread2:pthread2 unlock the variable\n");
     }
timer_set();

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
//static int last_pk_id=0;
static u_char buff_send[65535];
static unsigned int buf_send_p=0;
static char sync_head[8];
static bool pkg_cont_flag =0;
static unsigned int pk_total_num =0;
static unsigned int last_pk_total_num =0;

static int next_ii=0;

static char last_pkg_data[20];
static bool flag_search_both_pkg=0;
static bool head_not_whole_flag=0;

	#ifdef CHECKSUM_ENABLE	
static int sum_r=0;
static int last_sum_r=0;
	#endif

static bool capture_process(void)
{
static int lost_num =0;


//int mm;
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
				//printf("iio_buffer_refill time out 1s\n");
				return 0;
				}
			}
			int sample_count=IIO_BUFFER_SIZE;

			char *gm_p = iio_buffer_start(rxbuf);
			//u_char *gm_p = iio_buffer_start(rxbuf);
//printf("iio_buffer_start(rxbuf) %x: iio_buffer_end(rxbuf) %x,iio_buffer_end(rxbuf) %x\n",iio_buffer_start(rxbuf),iio_buffer_end(rxbuf),iio_buffer_step(rxbuf));
if(flag_search_both_pkg ==1)
{
int kkk =0;
memcpy(last_pkg_data+8,gm_p,8);
//char * last_pkg_data_p =last_pkg_data;
			for(;kkk<8;kkk++)
			{
				if(strncmp(last_pkg_data+kkk,sync_head,8)==0)
				{
				pk_total_num= *(gm_p+kkk)+((*(gm_p+kkk+1))<<8);
	#ifdef CHECKSUM_ENABLE	
    sum_r =  *(gm_p+kkk+2)+((*(gm_p+kkk+3))<<8);
	#endif
				next_ii=kkk+8;
				pkg_cont_flag=1;
//int mmm=0;
//for(;mmm<17;mmm++)
//printf("both_serch%d,:%x\n",mmm,*(last_pkg_data+mmm));
//printf("%%%%%%%%%%%%%%%%%%%%%%%%%\n");
//mmm=0;
//for(;mmm<17;mmm++)
//printf("gm_p block %d,:%x\n",mmm,*(gm_p+mmm));
//printf("xxxxxxall num:%d\n",pk_total_num);
				break;
				}
			}
flag_search_both_pkg =0;
}

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
				//printf("sync_head found:%d\n",k);
				break;
				}
				gm_p++;
			}
			if(k==sample_count*IIO_BUFFER_BUS_WIDTHS-8)
			{
			lost_num++;
			//printf("sync_head lost:%d\n",lost_num);
memcpy(last_pkg_data,gm_p,8);
flag_search_both_pkg =1;

			return 0;
			}
//printf("head hex:%x,%x\n",*(gm_p+8),*(gm_p+9));

//		pk_total_num= *((short *)gm_p+5+4);
//pk_total_num= *((short *)gm_p+5+4);
			//	int this_pk_num=*((short *)gm_p+6);
			//	int packet_id= *((short *)gm_p+7);
			//	sum_r = *((short *)gm_p+8);

				//printf("all num:%d\n",pk_total_num);
				//printf("this packet:%d\n",this_pk_num);
				//printf("packet id:%d\n",packet_id);
	if(k+9>=sample_count*IIO_BUFFER_BUS_WIDTHS)
	{
//pk_total_num=0;
//next_ii=0;
//pkg_cont_flag=0;
printf("pk_total_num set wrong!%d.\n",k+8);
//return 0;
	}
	else{
	pk_total_num= *(gm_p+8)+((*(gm_p+9))<<8);
			#ifdef CHECKSUM_ENABLE	
		if(k+11>=sample_count*IIO_BUFFER_BUS_WIDTHS)
		{
		printf("checksum error!%d.\n",k+8);
		//sum_r =0;
		}else
		{
		    sum_r = *(gm_p+10)+((*(gm_p+11))<<8);
		}
			#endif
	}

	ii=k+16;

		if(ii>=sample_count*IIO_BUFFER_BUS_WIDTHS)
		{
			next_ii=ii%(sample_count*IIO_BUFFER_BUS_WIDTHS);
			if(next_ii==8)
			{}
			else if (next_ii==7)
			{
			last_pk_total_num= *(gm_p+8);///length in next pkg
			}
			else if (next_ii==6)
			{
			//last_pk_total_num= *(gm_p+8)+((*(gm_p+9))<<8);
			}
	#ifdef CHECKSUM_ENABLE
			else if (next_ii==5)
			{
			last_sum_r = *(gm_p+10);//sum_r in next pkg
			}
	#endif
			head_not_whole_flag =1;
			flag_search_both_pkg =0;
			pkg_cont_flag=1;
			return 0;
		}
		else
		{
		next_ii=0;
		}
	gm_p=gm_p+16;
}
else
{
if(head_not_whole_flag==1)
{
			if(next_ii==8)
			{
			pk_total_num= *(gm_p)+((*(gm_p+1))<<8);
	#ifdef CHECKSUM_ENABLE
		    	sum_r = *(gm_p+2)+((*(gm_p+3))<<8);
	#endif
			}
			else if (next_ii==7)
			{
			//last_pk_total_num= *(gm_p+8);///length in next pkg
			pk_total_num= last_pk_total_num+((*(gm_p))<<8);
	#ifdef CHECKSUM_ENABLE
		    	sum_r = *(gm_p+1)+((*(gm_p+2))<<8);
	#endif
			}
	#ifdef CHECKSUM_ENABLE
			else if (next_ii==6)
			{
			//last_pk_total_num= *(gm_p+8)+((*(gm_p+9))<<8);
		    	sum_r = *(gm_p)+((*(gm_p+1))<<8);
			}
			else if (next_ii==5)
			{
			//last_sum_r = *(gm_p+10);//sum_r in next pkg
		    	sum_r = last_sum_r+((*(gm_p+1))<<8);
			}
	#endif
head_not_whole_flag=0;
}

	ii=next_ii;
	gm_p=gm_p+ii;
	next_ii=0;
}

if((pk_total_num>1560)||(pk_total_num<50))
{
	printf("length too large or too short,%d.\n",pk_total_num);
	buf_send_p=0;
pkg_cont_flag =0;
next_ii=0;
	#ifdef CHECKSUM_ENABLE
sum_r =0;
	#endif
flag_search_both_pkg =0;
head_not_whole_flag=0;
	return !stop_capture;
}
		


				for(;(ii<sample_count*IIO_BUFFER_BUS_WIDTHS)&&(buf_send_p<pk_total_num);ii++)
				{
				//if((ii<6)||(ii>sample_count*2-3))
				//printf("data count %d: value %d\n",ii,*(gm_p));
		

					buff_send[buf_send_p]=*(gm_p);
					buf_send_p++;


				gm_p++;
				}
	//printf("buf_send_p,%d\n",buf_send_p);

//printf("data ii %d,pk_total_num:%d\n",ii,pk_total_num);

				if(buf_send_p==pk_total_num)
				{

	#ifdef CHECKSUM_ENABLE	
    int sum_calc;
    sum_calc = 0;
    
            sum_calc = do_checksum_math((uint16_t *)buff_send, pk_total_num);
            sum_calc = CHECKSUM_CARRY(sum_calc);	
	if(sum_r != sum_calc)
	{
	printf("checksum crc received wrong,not equal\n");
	buf_send_p=0;
pkg_cont_flag =0;
next_ii=0;
sum_r =0;
flag_search_both_pkg =0;
head_not_whole_flag=0;
	return !stop_capture;
	}
	#endif


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


char gg;
gg=buff_send[29];
//buff_send[29]=buff_send[33];
buff_send[29]=0x58;
buff_send[33]=0x4d;

*/
///dest ip
//if(buff_send[33]==22)
//buff_send[33]=44;
//if(pk_total_num<1512)
//printf("all num:%d\n",pk_total_num);

//				printf("data ii %d,pk_total_num:%d\n",ii,pk_total_num);

//if(buff_send[29]!=22)
				pcap_inject(device_eth0,buff_send,pk_total_num);
//int inject_num=pcap_inject(device_eth0,buff_send,pk_total_num);
				//printf("send out datanum: %d,id:%d\n",inject_num,packet_id);
				buf_send_p=0;
pkg_cont_flag =0;
if(ii<sample_count*IIO_BUFFER_BUS_WIDTHS-8)
{
k=ii;
goto RECAPTURE;
}else if(ii<sample_count*IIO_BUFFER_BUS_WIDTHS)
{
memcpy(last_pkg_data,gm_p-(8-(sample_count*IIO_BUFFER_BUS_WIDTHS-ii)),8);
flag_search_both_pkg =1;

			return 0;
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



static void usage(char *program)
{
	printf("%s: the dma dual direction transmit and receive function\n", program);


	/* please keep this list sorted in alphabetical order */
	printf( "Command line options:\n"
		"\t-r\ttransmit frequency (MHz)\n"
		"\t-t\treceive frequency (MHz)\n");


	exit(-1);
}

void my_signal_func(int signum)
{
printf("SIGIO EMITS\n");
}



/* simple configuration and streaming */
int main (int argc, char **argv)
{
int rx_freq=2400;
int tx_freq=2400;
int bandwidth=18;
int gain=0;
/////gain need to be negative for example -10
	int c;
//	opterr = 0;
	while ((c = getopt (argc, argv, "r:t:b:g:?")) != -1)
		switch (c) {
			case 'r':
rx_freq =atoi(optarg);
printf("freq rx:%d MHz\n",rx_freq);
				break;
			case 't':
tx_freq =atoi(optarg);
printf("freq tx:%d MHz\n",tx_freq);
				break;

			case 'b':
bandwidth =atoi(optarg);
printf("bandwidth:%d MHz\n",bandwidth);
				break;

			case 'g':
gain =atoi(optarg);
printf("gain:%d\n",gain);
				break;

			case '?':
				usage(argv[0]);
				break;
			default:
				printf("Unknown command line option\n");
				usage(argv[0]);
				break;
		}

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
//	rxcfg.bw_hz = MHZ(32);   // 2 MHz rf bandwidth
rxcfg.bw_hz = MHZ(bandwidth); 
	rxcfg.fs_hz = MHZ(61.44);   // 2.5 MS/s rx sample rate
	rxcfg.lo_hz = MHZ(rx_freq);// 2.4 GHz rf frequency
	rxcfg.rfport = "A_BALANCED"; // port A (select for rf freq.)
rxcfg.gain_control_mode = "fast_attack";

	// TX stream config
//	txcfg.bw_hz = MHZ(32); // 1.5 MHz rf bandwidth
txcfg.bw_hz = MHZ(bandwidth); 
	txcfg.fs_hz = MHZ(61.44);   // 2.5 MS/s tx sample rate
	txcfg.lo_hz = MHZ(tx_freq); // 2.5 GHz rf frequency
	txcfg.rfport = "A"; // port A (select for rf freq.)
//txcfg.hardwaregain = 0;
txcfg.hardwaregain = gain;

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

reset_qpsk_rx();
usleep(1000);
unreset_qpsk_rx();

unsigned int fpga_version = rd_fpag_version();
printf("fpga version %x\n",fpga_version);

   /*互斥锁初始化*/
     pthread_mutex_init(&mutex,NULL);


printf("iio_device_get_sample_size  %d\n",iio_device_get_sample_size(tx));
open_eth0();
//open_eth1();



	uio_fd = open("/dev/mwipcore2", O_RDWR);
	if(uio_fd < 1)
	{
		printf("error: invalid uio_fd\n\r");
		return -1;
	}

	uio_addr = mmap(NULL, 0x1000, PROT_READ|PROT_WRITE, MAP_SHARED, uio_fd, 0);

signal(SIGIO,my_signal_func);

fcntl(uio_fd,F_SETOWN,getpid());
int Oflags = fcntl(uio_fd,F_GETFL);
fcntl(uio_fd,F_SETFL,Oflags | FASYNC);


dds_buffer_gm =txbuf;

g_thread_new("pcap loop", (void *) &always_loop, NULL);

	printf("* Starting IO streaming (press CTRL+C to cancel)\n");
timer_start();
timer_set();

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
