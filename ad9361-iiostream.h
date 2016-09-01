

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <iio.h>

#include <unistd.h>

/* helper macros */
#define MHZ(x) ((long long)(x*1000000.0 + .5))
#define GHZ(x) ((long long)(x*1000000000.0 + .5))

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
FILE *infile;

//#define NUM_PUSH_BUF 260000/16
#define NUM_PUSH_BUF 16250

/* cleanup and exit */
void shutdown();


void handle_sig(int sig);






/* finds AD9361 streaming IIO devices */
bool get_ad9361_stream_dev(struct iio_context *ctx, enum iodev d, struct iio_device **dev);

/* finds AD9361 streaming IIO channels */
bool get_ad9361_stream_ch(struct iio_context *ctx, enum iodev d, struct iio_device *dev, int chid, struct iio_channel **chn);



/* applies streaming configuration through IIO */
bool cfg_ad9361_streaming_ch(struct iio_context *ctx, struct stream_cfg *cfg, enum iodev type, int chid);





/* simple configuration and streaming 
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
	rxcfg.bw_hz = MHZ(2.5);   // 2 MHz rf bandwidth
	rxcfg.fs_hz = MHZ(2.6);   // 2.5 MS/s rx sample rate
	rxcfg.lo_hz = GHZ(0.9); // 2.5 GHz rf frequency
	rxcfg.rfport = "A_BALANCED"; // port A (select for rf freq.)

	// TX stream config
	txcfg.bw_hz = MHZ(2.5); // 1.5 MHz rf bandwidth
	txcfg.fs_hz = MHZ(2.6);   // 2.5 MS/s tx sample rate
	txcfg.lo_hz = GHZ(1.57542); // 2.5 GHz rf frequency
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
	//iio_channel_enable(rx0_i);
	//iio_channel_enable(rx0_q);
	iio_channel_enable(tx0_i);
	iio_channel_enable(tx0_q);

	txbuf = iio_device_create_buffer(tx, NUM_PUSH_BUF, false);
	if (!txbuf) {
		perror("Could not create TX buffer");
		shutdown();
	}

	printf("* Starting IO streaming (press CTRL+C to cancel)\n");


//FILE *infile;
char file_name_gm[80]="/media/boot/gaoming/gps120_xly.mat";
//struct iio_buffer *dds_buffer_gm;

long lSize;
long kk=0;
printf("gaoming002,%s\n",file_name_gm);
	infile = fopen(file_name_gm, "r");
fseek(infile,0,SEEK_END);
lSize=ftell(infile);
//rewind(infile);
printf("gaoming003,%ld,%ld\n",lSize,lSize/NUM_PUSH_BUF/4);
printf("gaoming006,%d\n",iio_device_get_sample_size(tx));

char *buf_ming;






	while (!stop)
	{

//always_gps_loop();
//iio_device_get_sample_size(tx)
rewind(infile);
kk=0;
for(;kk<lSize/NUM_PUSH_BUF/4;kk++)
{
	buf_ming=iio_buffer_start(txbuf);
		if(fread(buf_ming,4,NUM_PUSH_BUF,infile)!=NUM_PUSH_BUF)
	{
		//ret=100;
printf("gaoming005\n");
		break;
	}
		iio_buffer_push(txbuf);
//printf("gaoming006\n");
usleep(300);
}
printf("gaoming007\n");
	}

	shutdown();

	return 0;
} 
*/
