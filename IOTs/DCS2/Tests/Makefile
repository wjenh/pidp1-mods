# Makefile for DCS2 diagnostic and test programs.
#
# Targets:
#   all          -- build all .rim files and dcstestharness (default)
#   harness      -- compile dcstestharness only
#   T01 ... T11  -- assemble one test program
#   clean        -- remove all generated files
#
# To run a test:
#   1. Start the harness in one terminal (see README.md for per-test commands).
#   2. Read-in load the .rim into the emulator and start it.
#   3. The typewriter prints PASS/FAIL for each step and a final summary.

CC      = gcc
CFLAGS  = -Wall -Wextra
AM1     = am1

# All test programs
AM1_SRCS = T01.am1 T02.am1 T03.am1 T04.am1 T05.am1 \
           T06.am1 T07.am1 T08.am1 T09.am1 T10.am1 T11.am1

AM1_RIMS = $(AM1_SRCS:.am1=.rim)

.PHONY: all harness clean \
        T01 T02 T03 T04 T05 T06 T07 T08 T09 T10 T11 \
        dcstest dcsecho

all: $(AM1_RIMS) dcstestharness

# ---- test harness ----------------------------------------------------------

harness: dcstestharness

dcstestharness: dcstestharness.c
	$(CC) $(CFLAGS) -o $@ $<

# ---- assembler targets -----------------------------------------------------

%.rim: %.am1
	$(AM1) -b $<

# Convenience phony targets: make T01 assembles T01.am1 only
T01: T01.rim
T02: T02.rim
T03: T03.rim
T04: T04.rim
T05: T05.rim
T06: T06.rim
T07: T07.rim
T08: T08.rim
T09: T09.rim
T10: T10.rim
T11: T11.rim

# ---- legacy targets (kept for backward compatibility) ----------------------

dcstest: dcstest.rim

dcstest.rim: dcstest.am1
	$(AM1) $<

dcsecho: dcsecho.c
	$(CC) $(CFLAGS) -o $@ $<

# ---- clean -----------------------------------------------------------------

clean:
	@rm -f $(AM1_RIMS) dcstestharness dcstest.rim dcsecho
	@rm -f *.lst *.sym
