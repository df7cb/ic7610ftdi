all: ic7610ftdi

CFLAGS += -Wall -g
LDLIBS += -lftd3xx

ic7610ftdi: ic7610ftdi.o
