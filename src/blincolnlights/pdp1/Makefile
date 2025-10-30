INC=-I..
LIBS=-lpthread -lm -lSDL2

all: pdp1_b18 pdp1

pdp1_b18: main.c panelb18.c pdp1.c typtelnet.c audio.c lowpass.c ../common.c ../pollfd.c dynamicIots.o \
    highSpeedChannels.o logger.o
	cc -g -O3 -o $@ $^ $(INC) $(LIBS)

pdp1: main.c panel1.c pdp1.c typtelnet.c audio.c lowpass.c ../common.c ../pollfd.c dynamicIots.o highSpeedChannels.o \
    logger.o
	gcc -g -O3 -Wl,--dynamic-list=symbol-exports.ldr -o $@ $^ $(INC) $(LIBS)

logger.o: logger.c logger.h
	cc -g -O3 -c logger.c $(INC)

dynamicIots.o: dynamicIots.c dynamicIots.h pdp1.h
	cc -g -O3 -c dynamicIots.c $(INC)

highSpeedChannels.o: highSpeedChannels.c highSpeedChannels.h pdp1.h
	cc -g -O3 -c highSpeedChannels.c $(INC)
