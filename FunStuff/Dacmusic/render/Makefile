# dacrender: a tape punched by the D/A converters' music program, to a wav file.
# C99 and the math library only. -ffp-contract=off keeps every product and
# sum rounded as written (no fused multiply-add), so the file is the same on
# every host. The room (room.h) and the voicing (edge.h) are the device's
# own, in the plugin's directory; ROOMDIR says where they are.

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
CFLAGS += -std=c99 -ffp-contract=off
ROOMDIR ?= /opt/pidp1-mods/IOTs/DAC

dacrender: dacrender.c dacrender_lib.h $(ROOMDIR)/room.h $(ROOMDIR)/edge.h
	$(CC) $(CFLAGS) -I$(ROOMDIR) -o dacrender dacrender.c -lm

clean:
	rm -f dacrender

.PHONY: clean
