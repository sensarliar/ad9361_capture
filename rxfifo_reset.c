


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
	//printf("r: reg[0x%x] = 0x%x\n\r", reg_addr, *reg_val);

	munmap(uio_addr, 0x1000);
	close(uio_fd);

	return 0;
}

int gm_rd(const char *dev,  unsigned int* reg_val)
{
	//int uio_fd;
FILE* uio_fd;
	//void *uio_addr;

//	uio_fd = fopen(dev, O_RDWR);

uio_fd = fopen(dev, "rb");

fread(reg_val,1,1,uio_fd);
	fclose(uio_fd);

//*reg_val=0;
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

unsigned int rd_fpag_version(void)
{
	unsigned int fpga_ver=0;
	uio_rd("/dev/version", 0x00, &fpga_ver);
	return fpga_ver;

}

/*
unsigned int rd_txfifo_hf_flag(void)
{
	unsigned int hf_flag=0;
//	uio_rd("/dev/mwipcore2", 0x00, &hf_flag);
//gm_rd("/dev/mwipcore2", &hf_flag);
	hf_flag = hf_flag&0x00000001;
	return hf_flag;

}
*/