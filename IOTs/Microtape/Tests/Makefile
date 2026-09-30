# Makefile for the Type 550 Microtape tests.
#
# Targets:
#   test          -- build and run mttest, the controller/transport unit tests (default)
#   mttest        -- build it only
#   plugintest    -- build and run the reader plugin with the tape in it: routing on device 01,
#                    microtapes.txt, the mount IOT 201, SIGHUP (plugintest.c)
#   roundtrip     -- build mkmicrotape and run its tests (roundtrip.sh)
#   probes        -- assemble the am1 probe programs (AM1 and AM1INC can be overridden)
#   clean         -- remove everything built or left behind
#
# IOTs/install.sh skips directories named Tests, so nothing here is built or installed by it.
#
# 11-Sep-2026 Claude -- plugintest and probe_mount added; the loader test plugins removed
#                       with the combined-code loader change.
# 13-Sep-2026 Claude -- probe_halt added.
# 30-Sep-2026 Claude -- the probes that do not ship with the release removed from PROBES.

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -I..

CORE    = ../control550.c ../transport555.c
HDRS    = ../control550.h ../transport555.h

PLUGIN_INC = -I../../../src/blincolnlights -I../../../src/blincolnlights/pdp1 -I../.. -I../../Lib -I..
PLUGIN_SRC = ../../Reader/IOT_2.c ../microtape.c $(CORE)

AM1     ?= am1
AM1INC  ?= ../../../Am1Includes
PROBES  = mtsample.am1 probe_halt.am1

.PHONY: test roundtrip probes clean

test: mttest
	./mttest

mttest: mttest.c $(CORE) $(HDRS)
	$(CC) $(CFLAGS) -o $@ mttest.c $(CORE)

# The plugin test builds with microtapes.txt and relative images in ./plugintest.d.
plugintest: plugintest.c $(PLUGIN_SRC) $(HDRS) ../microtape.h ../../iotHandler.h ../../../src/blincolnlights/pdp1/dynamicIots.h ../../../src/blincolnlights/pdp1/pdp1.h ../../../src/blincolnlights/pdp1/ad1intf.h ../../../src/blincolnlights/pdp1/configuration.h ../../Lib/iotLogger.h ../../Lib/writeBehind.h
	$(CC) $(PLUGIN_INC) -Wall -O2 '-DMT_BASE_DIR="plugintest.d"' -o $@ plugintest.c $(PLUGIN_SRC)
	./plugintest

mkmicrotape: ../mkmicrotape.c ../transport555.c ../transport555.h
	$(CC) $(CFLAGS) -o $@ ../mkmicrotape.c ../transport555.c

roundtrip: mkmicrotape
	sh ./roundtrip.sh ./mkmicrotape

probes:
	for f in $(PROBES); do $(AM1) -S -l -i $(AM1INC) $$f || exit 1; done

clean:
	@rm -rf mttest plugintest plugintest.d mkmicrotape mttest-*.img rt-*.img rt-*.dt *.rim *.lst *.sym
