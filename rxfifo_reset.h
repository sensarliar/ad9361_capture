
int uio_wr(const char *dev, unsigned int reg_addr, unsigned int reg_val);


int uio_rd(const char *dev, unsigned int reg_addr, unsigned int* reg_val);




void reset_qpsk_rx(void);


void unreset_qpsk_rx(void);


unsigned int rd_txfifo_hf_flag(void);

