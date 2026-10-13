# dacvoice: a table voice or an organ stop for the D/A converters' music
# program, from a definition. dacdrum: a library of voice files to the
# Type 23 drum file, for the player to load. C99 and the math library only. -ffp-contract=off
# keeps every product and sum rounded as written (no fused multiply-add),
# so a voice is the same on every host.

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
CFLAGS += -std=c99 -ffp-contract=off

all: dacvoice dacdrum

dacvoice: dacvoice.c
	$(CC) $(CFLAGS) -o dacvoice dacvoice.c -lm

dacdrum: dacdrum.c
	$(CC) $(CFLAGS) -o dacdrum dacdrum.c

clean:
	rm -f dacvoice dacdrum

.PHONY: all clean
