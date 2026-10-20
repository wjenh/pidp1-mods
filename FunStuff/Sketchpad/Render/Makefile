# GNU make predefines CC as cc, so "CC ?= gcc" would never take; a CC from the environment or
# the command line still wins.
ifeq ($(origin CC),default)
CC = gcc
endif
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11

../sketchplot: sketchplot.c
	$(CC) $(CFLAGS) -o sketchplot sketchplot.c -lm
	mv sketchplot ..

clean:
	rm -f ../sketchplot

.PHONY: clean
