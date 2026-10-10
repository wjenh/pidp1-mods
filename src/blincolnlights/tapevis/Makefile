# Every file tapevis.c and common.c include, from gcc -MM -I.. tapevis.c ../common.c.
DEPS = ../common.h ../args.h

all: tapevis

tapevis: tapevis.c ../common.c $(DEPS)
	cc -o $@ tapevis.c ../common.c -I.. `sdl2-config --cflags --libs`
