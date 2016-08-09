

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
#include "rxfifo_reset.h"

int uio_wr(const char *dev, unsigned int reg_addr, unsigned int reg_val)
{
	int uio_fd;
	void *uio_addr;

	uio_fd = open(dev, O_RDWR);
	if(uio_fd < 1)
	{
		printf("error: invalid uio_fd\n\r");
		return -1;
	}

	uio_addr = mmap(NULL, 0x1000, PROT_READ|PROT_WRITE, MAP_SHARED, uio_fd, 0);
	*((unsigned *) (uio_addr + reg_addr)) = reg_val;

	munmap(uio_addr, 0x1000);
	close(uio_fd);

	return 0;
}

int uio_rd(const char *dev, unsigned int reg_addr, unsigned int* reg_val)
{
	int uio_fd;
	void *uio_addr;

	uio_fd = open(dev, O_RDWR);
	if(uio_fd < 1)
	{
		printf("error: invalid uio_fd\n\r");
		return -1;
	}

	uio_addr = mmap(NULL, 0x1000, PROT_READ|PROT_WRITE, MAP_SHARED, uio_fd, 0);

	*reg_val = *((unsigned *) (uio_addr + reg_addr));
	printf("r: reg[0x%x] = 0x%x\n\r", reg_addr, *reg_val);

	munmap(uio_addr, 0x1000);
	close(uio_fd);

	return 0;
}



void reset_qpsk_rx(void)
{

	uio_wr("/dev/mwipcore", 0x00, 0x01);
}


void unreset_qpsk_rx(void)
{

	uio_wr("/dev/mwipcore", 0x00, 0x00);
}

