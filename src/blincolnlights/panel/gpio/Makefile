CC=gcc -O3 -Wno-unused-result
INC=

# 16-Jul-2026 wje (Claude) - real per-file dependency tracking (source-tree refactor phase
# 3, see REFACTOR-CHANGES.txt at the repo root). Previously "all: $(OBJ)" listed the .o
# files as prerequisites, so make's implicit rule built each one individually, but the
# recipe attached to "all" then unconditionally recompiled every source together again on
# every single invocation regardless of what had actually changed, and there was no
# -MMD/-MP, so a shared header change (gpiochip.h, gpiolib.h, util.h) did not force a
# rebuild of whichever .c files depended on it. Now each .c gets its own object with real
# header dependency tracking, and "all" only rebuilds what is actually stale.
#
# 17-Jul-2026 wje (Claude) - source-tree refactor phase 4: this directory moved from
# panel_pidp1/Pinctrl/ to panel/gpio/ (a sibling of panel/driver/ now, not a child of the
# old panel_pidp1/). Entirely self-contained (wildcard *.c, no relative-path sources or
# includes), so no path changes were needed here beyond the move itself. panel/driver/'s
# Makefile now reaches this directory via "../gpio" instead of "Pinctrl".
SRC := $(wildcard *.c)
OBJ := $(SRC:.c=.o)
DEPS := $(OBJ:.o=.d)

.PHONY: all clean

all: $(OBJ)

%.o: %.c
	$(CC) -c $< $(INC) -MMD -MP -o $@

# Auto-generated header dependencies, one per object file, produced by -MMD/-MP above. The
# leading "-" means a missing .d (e.g. before the first build has ever run) is not an error.
-include $(DEPS)

clean:
	rm -f $(OBJ) $(DEPS)
