# Every file tapevis.c and common.c include, from gcc -MM -I../common tapevis.c ../common/common.c.
DEPS = ../common/common.h ../common/args.h

all: tapevis

tapevis: tapevis.c ../common/common.c $(DEPS)
	cc -o $@ tapevis.c ../common/common.c -I../common `sdl2-config --cflags --libs`
