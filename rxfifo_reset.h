#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <iio.h>
#include <math.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
int uio_wr(const char *dev, unsigned int reg_addr, unsigned int reg_val);


int uio_rd(const char *dev, unsigned int reg_addr, unsigned int* reg_val);




void reset_qpsk_rx(void);


void unreset_qpsk_rx(void);


//unsigned int rd_txfifo_hf_flag(void);
unsigned int rd_fpag_version(void);

