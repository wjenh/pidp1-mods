#CC=gcc
CC=gcc -w
INC=-I..
LIBS=-lpthread -lm -lSDL2

all: pdp1

pdp1: main.c panel1.c pdp1.c typtelnet.c audio.c lowpass.c ../common.c ../pollfd.c dynamicIots.o highSpeedChannels.o \
    ../logger.c
	$(CC) -g -O3 -Wl,--dynamic-list=symbol-exports.ldr -o $@ $^ $(INC) $(LIBS)

dynamicIots.o: dynamicIots.c dynamicIots.h pdp1.h
	$(CC) -g -O3 -c dynamicIots.c $(INC)

highSpeedChannels.o: highSpeedChannels.c highSpeedChannels.h pdp1.h
	$(CC) -g -O3 -c highSpeedChannels.c $(INC)
