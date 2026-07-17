CC=gcc -O3 -Wall -Wextra
#CC=gcc -g -Wall -Wextra

INC=-I../.. -I../../pdp1 -I../gpio
LIBS=-lpthread -lm

# 16-Jul-2026 wje (Claude) - brought into line with pdp1/Makefile's dependency-tracking
# pattern (source-tree refactor phase 3, see REFACTOR-CHANGES.txt at the repo root).
# Previously newpanel.o/common.o/configuration.o each had a hand-written compile rule with
# no header dependency tracking, so a change to a shared header (common.h, panel_pidp1.h,
# configuration.h) did not force a rebuild of anything that included it. Every object now
# gets -MMD/-MP so gcc discovers header dependencies itself, matching pdp1/Makefile.
# Add a new source in this directory to LOCAL_SRCS, a new one from the parent directory
# (blincolnlights/) to PARENT_SRCS, or a new one from ../../pdp1 to PDP1_SRCS. Nothing else
# needs to change.
#
# 17-Jul-2026 wje (Claude) - source-tree refactor phase 4: this directory moved from
# panel_pidp1/ to panel/driver/ (one level deeper under blincolnlights/), and the sibling
# Pinctrl/ subdirectory moved out to panel/gpio/ (now a sibling of this directory instead
# of a child). INC/PARENT_SRCS/PDP1_SRCS paths gained an extra "../" for the added depth,
# and every "Pinctrl" reference below became "../gpio". Functionally unchanged otherwise.
LOCAL_SRCS = newpanel.c
PARENT_SRCS = ../../common.c
PDP1_SRCS = ../../pdp1/configuration.c

OBJS = $(LOCAL_SRCS:.c=.o) $(notdir $(PARENT_SRCS:.c=.o)) $(notdir $(PDP1_SRCS:.c=.o))
DEPS = $(OBJS:.o=.d)

.PHONY: all clean gpio run

all: gpio newpanel

newpanel: gpio $(OBJS)
	$(CC) $(INC) -o $@ $(OBJS) ../gpio/*.o $(LIBS)

# Compile rule for sources in this directory.
%.o: %.c
	$(CC) -c $< $(INC) -MMD -MP -o $@

# Compile rule for sources living in the parent directory (blincolnlights/).
%.o: ../../%.c
	$(CC) -c $< $(INC) -MMD -MP -o $@

# Compile rule for sources living in ../../pdp1.
%.o: ../../pdp1/%.c
	$(CC) -c $< $(INC) -MMD -MP -o $@

# Auto-generated header dependencies, one per object file, produced by -MMD/-MP above. The
# leading "-" means a missing .d (e.g. before the first build has ever run) is not an error.
-include $(DEPS)

run: newpanel
	./newpanel

# Recursive make into ../gpio is unconditional (phony), but its own Makefile only rebuilds
# its own stale .o files rather than recompiling everything on every invocation, so this
# stays cheap when nothing there changed.
gpio:
	cd ../gpio; make

clean:
	rm -f $(OBJS) $(DEPS) newpanel
	cd ../gpio; make clean
