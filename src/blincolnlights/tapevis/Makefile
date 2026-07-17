CC=gcc

INC=-I..
SDL2_CFLAGS := $(shell sdl2-config --cflags)
SDL2_LIBS := $(shell sdl2-config --libs)

# 17-Jul-2026 wje (Claude) - brought into line with pdp1/Makefile's dependency-tracking
# pattern (source-tree refactor phase 3, see REFACTOR-CHANGES.txt at the repo root).
# Previously tapevis.o/common.o had a single hand-written link recipe (raw .c files fed
# straight into the link line, "cc -o $@ $^ ...") with no header dependency tracking, so a
# change to a shared header (common.h) did not force a rebuild. Every object now gets a
# real .o with a real timestamp, and -MMD/-MP let gcc discover header dependencies itself.
# sdl2-config's --cflags/--libs are split into separate variables since --cflags belongs on
# the per-file compile rules and --libs only on the final link.
# Add a new source in this directory to LOCAL_SRCS, a new one from the parent directory
# (blincolnlights/) to PARENT_SRCS. Nothing else needs to change.
LOCAL_SRCS = tapevis.c
PARENT_SRCS = ../common.c

OBJS = $(LOCAL_SRCS:.c=.o) $(notdir $(PARENT_SRCS:.c=.o))
DEPS = $(OBJS:.o=.d)

.PHONY: all clean

all: tapevis

tapevis: $(OBJS)
	$(CC) -o $@ $(OBJS) $(SDL2_LIBS)

# Compile rule for sources in this directory.
%.o: %.c
	$(CC) -c $< $(INC) $(SDL2_CFLAGS) -MMD -MP -o $@

# Compile rule for sources living in the parent directory (blincolnlights/).
%.o: ../%.c
	$(CC) -c $< $(INC) $(SDL2_CFLAGS) -MMD -MP -o $@

# Auto-generated header dependencies, one per object file, produced by -MMD/-MP above. The
# leading "-" means a missing .d (e.g. before the first build has ever run) is not an error.
-include $(DEPS)

clean:
	rm -f $(OBJS) $(DEPS) tapevis
