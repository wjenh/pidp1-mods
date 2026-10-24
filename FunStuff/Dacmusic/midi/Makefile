# midi2dac turns a Standard MIDI File into a tape for the D/A converters' music
# player; midi2hc turns one into source text for the Harmony Compiler. C99 and
# the C library only. -ffp-contract=off keeps every product and sum rounded as
# written (no fused multiply-add), so a tape or a text is the same on every host.

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
CFLAGS += -std=c99 -ffp-contract=off

all: midi2dac midi2hc

midi2dac: midi2dac.c
	$(CC) $(CFLAGS) -o midi2dac midi2dac.c

midi2hc: midi2hc.c
	$(CC) $(CFLAGS) -o midi2hc midi2hc.c

clean:
	rm -f midi2dac midi2hc

.PHONY: all clean
