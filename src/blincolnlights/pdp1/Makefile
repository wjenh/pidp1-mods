#CC=gcc
CC=gcc -w
INC=-I..
LIBS=-lpthread -lm -lSDL2

all: pdp1

pdp1: main.c panel1.c typtelnet.c audio.c lowpass.c ../common.c ../pollfd.c pdp1.o dynamicIots.o highSpeedChannels.o \
    ../logger.c
	$(CC) -g -O3 -Wl,--dynamic-list=symbol-exports.ldr -o $@ $^ $(INC) $(LIBS)

pdp1.o: pdp1.c pdp1.h ad1intf.h dynamicIots.h ../logger.h
	$(CC) -g -O3 -c pdp1.c $(INC)

audio.o: audio.c audio.h lowpass.h pdp1.h ad1intf.h
	$(CC) -g -O3 -c audio.c $(INC)

lowpass.o: lowpass.c pdp1.h lowpass.h ad1intf.h
	$(CC) -g -O3 -c lowpass.c $(INC)

dynamicIots.o: dynamicIots.c dynamicIots.h pdp1.h ad1intf.h
	$(CC) -g -O3 -c dynamicIots.c $(INC)

highSpeedChannels.o: highSpeedChannels.c highSpeedChannels.h pdp1.h ad1intf.h
	$(CC) -g -O3 -c highSpeedChannels.c $(INC)
