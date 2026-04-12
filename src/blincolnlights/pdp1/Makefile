#CC=gcc -w -O3
CC=gcc -w -g

INC=-I..
LIBS=-lpthread -lm -lSDL2

all: pdp1

pdp1: main.c panel1.c typtelnet.c audio.c lowpass.c ../common.c ../pollfd.c pdp1.o dynamicIots.o highSpeedChannels.o \
    ../logger.c configuration.o display.o
	$(CC) -Wl,--dynamic-list=symbol-exports.ldr -o $@ $^ $(INC) $(LIBS)

pdp1.o: pdp1.c pdp1.h ad1intf.h dynamicIots.h ../logger.h
	$(CC) -c pdp1.c $(INC)

display.o: display.c pdp1.h pdp1.h ../common.h
	$(CC) -c display.c $(INC)

configuration.o: configuration.c configuration.h
	$(CC) -c configuration.c $(INC)

audio.o: audio.c audio.h lowpass.h pdp1.h ad1intf.h ../common.h
	$(CC) -c audio.c $(INC)

lowpass.o: lowpass.c pdp1.h lowpass.h ad1intf.h
	$(CC) -c lowpass.c $(INC)

dynamicIots.o: dynamicIots.c dynamicIots.h pdp1.h ../common.h ad1intf.h
	$(CC) -c dynamicIots.c $(INC)

highSpeedChannels.o: highSpeedChannels.c highSpeedChannels.h pdp1.h ../common.h ad1intf.h
	$(CC) -c highSpeedChannels.c $(INC)
