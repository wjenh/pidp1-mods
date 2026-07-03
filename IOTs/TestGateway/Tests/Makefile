# Makefile for the HSC Test Gateway (IOT 44) test programs.
#
# Targets:
#   all          -- assemble all .rim files (default)
#   T01 ... T05  -- assemble one test program
#   harness      -- build hscharness, the standalone C priority-arbitration test
#   clean        -- remove all generated files
#
# To run a test: rim-load the .rim file into the emulator (with IOT_44.so installed,
# see ../README.txt) and start it. The typewriter prints PASS/FAIL for each step and a
# final summary. See README.md for what each test covers.
#
# Like IOTs/DCS2/Tests/Makefile, this assumes `am1` on $PATH already knows its include
# root (the -i flag / am1's own default -- see `am1 -h`). If it doesn't in your
# environment, override on the command line, e.g.:
#   make AM1="am1 -i /path/to/pidp1-mods/Am1Includes"

CC      = gcc
CFLAGS  = -Wall -Wextra
AM1     = am1

AM1_SRCS = T01.am1 T02.am1 T03.am1 T04.am1 T05.am1
AM1_RIMS = $(AM1_SRCS:.am1=.rim)

PDP1DIR = ../../../src/blincolnlights/pdp1
PDP1INC = -I../../../src/blincolnlights -I$(PDP1DIR)

.PHONY: all harness clean T01 T02 T03 T04 T05

all: $(AM1_RIMS)

%.rim: %.am1
	$(AM1) $<

T01: T01.rim
T02: T02.rim
T03: T03.rim
T04: T04.rim
T05: T05.rim

# ---- standalone C priority-arbitration harness -----------------------------
#
# Links highSpeedChannels.c directly, bypassing the emulator binary/SDL/dlopen entirely --
# see hscharness.c's header comment for why this is the only way to test true multi-channel
# priority arbitration at all.

harness: hscharness

hscharness: hscharness.c $(PDP1DIR)/highSpeedChannels.c $(PDP1DIR)/highSpeedChannels.h
	$(CC) $(CFLAGS) $(PDP1INC) -o $@ hscharness.c $(PDP1DIR)/highSpeedChannels.c -lpthread -lrt

clean:
	@rm -f $(AM1_RIMS) hscharness
	@rm -f *.lst *.sym
