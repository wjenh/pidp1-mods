# Makefile for the plotter plugin's host tests (no emulator).
#
# Targets:
#   test        -- T1 then the plugin tests (default)
#   pngtest     -- T1: encode known bitmaps, check them and checkpng.py's own controls
#   plugintest  -- T2-T6: the plugin against a stand-in emulator, then its pictures
#   plottest    -- assemble plottest.am1, the emulator test (AM1 and AM1INC can be overridden)
#   clean       -- remove everything built or left behind
#
# IOTs/install.sh skips directories named Tests, so nothing here is built or installed by it.

CC      = gcc
CFLAGS  = -Wall -Wextra -O2
PYTHON  ?= python3

PLUGIN_INC = -I../../../src/blincolnlights -I../../../src/blincolnlights/pdp1 -I../.. -I../../Lib -I..
PLUGIN_HDRS = ../IOT_73.c ../pngWrite.h ../../iotHandler.h ../../../src/blincolnlights/pdp1/dynamicIots.h ../../../src/blincolnlights/pdp1/pdp1.h ../../../src/blincolnlights/pdp1/ad1intf.h ../../../src/blincolnlights/pdp1/configuration.h ../../Lib/iotLogger.h ../../Lib/writeBehind.h

AM1     ?= am1
AM1INC  ?= ../../../Am1Includes

.PHONY: test pngtest plugintest plottest clean

test: pngtest plugintest

pngtest: pngtest.c ../pngWrite.c ../pngWrite.h
	$(CC) $(CFLAGS) -I.. -o $@.bin pngtest.c ../pngWrite.c -lz
	rm -rf t1.d
	./$@.bin t1.d
	$(PYTHON) -B checkpng.py --manifest t1.d/manifest.txt
	$(PYTHON) -B checkpng.py --selftest t1.d/sheet560.png t1.d/sheet560.pbm 3937
	$(PYTHON) -B checkpng.py --selftest t1.d/width13.png t1.d/width13.pbm 3937

# Built with logging on, so the busy report reaches /tmp/iot.dbg, which T4 reads. The logger is
# Lib's object: iotLogger.c defines DOLOGGING itself, and -DDOLOGGING would redefine it.
plugintest: plugintest.c $(PLUGIN_HDRS) ../pngWrite.c ../../Lib/iotLogger.o ../../Lib/writeBehind.o
	$(CC) $(PLUGIN_INC) $(CFLAGS) -DDOLOGGING -o $@.bin plugintest.c ../pngWrite.c ../../Lib/iotLogger.o ../../Lib/writeBehind.o -lz -lpthread -lm
	rm -rf plugintest.d
	./$@.bin
	$(PYTHON) -B checkpng.py --manifest plugintest.d/manifest.txt

# Lib's objects are built by its own Makefile, which knows their headers.
../../Lib/%.o: FORCE
	$(MAKE) -C ../../Lib $*.o

FORCE:

plottest:
	$(AM1) -S -l -s -i $(AM1INC) plottest.am1

clean:
	@rm -rf pngtest.bin plugintest.bin t1.d plugintest.d *.rim *.lst *.sym
