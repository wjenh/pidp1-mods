CC=gcc -O3
INC=-I..
LIBS=-lSDL2 -lSDL2_image

# Builds the vpanel_pdp1 SDL2 virtual front panel. Links main.c (elements.inc is
# #include'd directly into it, not compiled separately) with the shared common.c support
# routines (createseg/panic/etc., see ../common.h) one directory up, against SDL2 +
# SDL2_image.
#
# 17-Jul-2026 wje (Claude) - brought into line with pdp1/Makefile's dependency-tracking
# pattern (source-tree refactor phase 3, see REFACTOR-CHANGES.txt at the repo root).
# Previously main.o/common.o had a single hand-written link recipe with no header
# dependency tracking, so a change to a shared header (common.h, panel_pidp1.h) did not
# force a rebuild. elements.inc is still picked up correctly: it is a real #include in
# main.c, so gcc's -MMD/-MP dependency scan sees it like any other header. Every object
# now gets a real .o with a real timestamp instead of being recompiled from a raw .c file
# fed straight into the link line.
# Add a new source in this directory to LOCAL_SRCS, a new one from the parent directory
# (blincolnlights/) to PARENT_SRCS. Nothing else needs to change.
LOCAL_SRCS = main.c
PARENT_SRCS = ../common.c

OBJS = $(LOCAL_SRCS:.c=.o) $(notdir $(PARENT_SRCS:.c=.o))
DEPS = $(OBJS:.o=.d)

.PHONY: all clean

all: panel_pdp1

panel_pdp1: $(OBJS)
	$(CC) -o $@ $(OBJS) $(LIBS)

# Compile rule for sources in this directory.
%.o: %.c
	$(CC) -c $< $(INC) -MMD -MP -o $@

# Compile rule for sources living in the parent directory (blincolnlights/).
%.o: ../%.c
	$(CC) -c $< $(INC) -MMD -MP -o $@

# Auto-generated header dependencies, one per object file, produced by -MMD/-MP above. The
# leading "-" means a missing .d (e.g. before the first build has ever run) is not an error.
-include $(DEPS)

clean:
	rm -f $(OBJS) $(DEPS) panel_pdp1
