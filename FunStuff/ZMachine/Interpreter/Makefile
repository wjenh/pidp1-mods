# Makefile -- the PDP-1 Z-machine interpreter.
#
# One image, zmachine.rim, for V3, V4 and V5 stories (.z3, .z4, .z5).
# Boot reads the story's version byte and sets the interpreter up for it;
# a story of any other version is refused.
#
#   make                          the image, and the story loader
#   make zmachine.rim             the image alone
#   make story STORY=file.z5      copy a story onto the drum (the whole drum
#                                 is rewritten; saved games are on the
#                                 tape mounted on drive 2, which this does
#                                 not touch)
#   make clean
#
# AM1        the assembler (default: am1 on PATH)
# AM1INCDIR  am1's include root, for its <...> includes
# DRUM       the drum file make story writes
# LPT64      must agree with lptType64 in pidp1.config: 0 is the Type 62
#            line printer (the default), 1 the Type 64
#
# am1 writes its output into the directory it runs in, so the image is
# assembled in $(BUILD). am1's exit status does not show a failed assembly;
# the "Bank 0," line of its -S table does, and it is also checked against
# bank 0's usable ceiling, 07750 (4072): am1's read-in loader occupies
# 07751-07774 while the tape loads.

AM1       = am1
AM1INCDIR ?= /opt/pidp1-mods/Am1Includes
DRUM      = /opt/pidp1-mods/pdp23drum
LPT64     = 0
LOADERDIR = ../Loader
LOADER    = $(LOADERDIR)/zloader
BUILD     = build_prod

SRCS = zmemcore.am1 $(wildcard *.ac)

.PHONY: all story clean

all: zmachine.rim $(LOADER)

install: all
	cp zmachine.rim ..

zmachine.rim: $(SRCS)
	@mkdir -p $(BUILD)
	@rm -f $(BUILD)/zmemcore.rim $(BUILD)/am1.out
	cd $(BUILD) && $(AM1) -S -i $(AM1INCDIR) -I $(AM1INCDIR)/UTIL \
	    -DZ_TEST_HARNESS=0 -DZLPT_TYPE64=$(LPT64) \
	    ../zmemcore.am1 > am1.out 2>&1; cat am1.out
	@high=`sed -n 's/^Bank 0, [0-7]* (\([0-9]*\) decimal).*$$/\1/p' $(BUILD)/am1.out`; \
	if [ -z "$$high" ] || [ ! -f $(BUILD)/zmemcore.rim ]; then \
	    echo "$@: am1 failed, no image (see $(BUILD)/am1.out)"; exit 1; fi; \
	if [ "$$high" -gt 4072 ]; then \
	    echo "$@: bank 0 reaches $$high (decimal), above 07750 (4072)," \
	         "into the read-in loader"; exit 1; fi; \
	echo "$@: bank 0 at $$high (decimal), `expr 4072 - $$high` words below 07750"
	mv $(BUILD)/zmemcore.rim $@

$(LOADER): $(LOADERDIR)/zloader.c $(LOADERDIR)/zloader.h
	cd $(LOADERDIR) && $(MAKE)

story: $(LOADER)
	@if [ -z "$(STORY)" ]; then \
	    echo "usage: make story STORY=path/to/story.z3|.z4|.z5"; exit 1; fi
	$(LOADER) -i $(DRUM) $(STORY)

clean:
	@rm -rf $(BUILD) zmachine.rim
