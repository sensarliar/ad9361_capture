# libiio - Library for interfacing industrial I/O (IIO) devices
#
# Copyright (C) 2014 Analog Devices, Inc.
# Author: Paul Cercueil <paul.cercueil@analog.com>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.


DEPENDENCIES := glib-2.0 gthread-2.0 

TARGETS := ad9361-capture

CFLAGS = -Wall
CFLAGS += `pkg-config --cflags $(DEPENDENCIES)`

LDFLAGS = `pkg-config --libs $(DEPENDENCIES)`

.PHONY: all clean

all: $(TARGETS)

#gcc -Wall -D_REENTRANT -D_POSIX_TIMERS timer.c -o timer -lrt
timer.o : timer.c
	$(CC) timer.c -c $(CFLAGS)  -D_REENTRANT -D_POSIX_TIMERS

rxfifo_reset.o : rxfifo_reset.c
	$(CC) rxfifo_reset.c -c $(CFLAGS)

ad9361-capture : ad9361-capture.o rxfifo_reset.o timer.o
	$(CC) -o $@ $^ $(LDFLAGS) -liio -lpcap -lpthread -lrt 

clean:
	rm -f $(TARGETS) $(TARGETS:%=%.o) rxfifo_reset.o timer.o
install: 
	cp $(TARGETS) ./../../gao
