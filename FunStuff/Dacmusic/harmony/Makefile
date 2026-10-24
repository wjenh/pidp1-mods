# Harmony, harmony.rim, is the Harmony Compiler for the PDP-1 music tapes,
# for the D/A converters' player and for pdp1m13.
# Start at 0101 for the console, at 0100 for its menu on the Type 340.
# See Docs/UsingHarmony.md.

SRCS= harmony.am1 hcread.ac hccomp.ac hcmenu.ac hcwrite.ac hcmacros.ah

all: harmony.rim

harmony.rim: $(SRCS)
	am1 harmony.am1

clean:
	@rm -f harmony.rim harmony.lst harmony.sym

.PHONY: all clean
