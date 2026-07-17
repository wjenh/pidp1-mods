# Archived 16-Jul-26: this used to live at panel_pidp1/Old/Makefile. Paths below updated for
# the new location (Archive/panel_pidp1_Old is two levels below blincolnlights, same depth as
# the old panel_pidp1/Old, but ../Pinctrl now needs an extra ../panel_pidp1 hop since the
# sibling directory is no longer panel_pidp1).
INC=-I../.. -I../../pdp1 -I../../panel_pidp1/Pinctrl
LIBS=-lpthread -lm
CFLAGS= $(INC) -O3 -Wno-unused-result

PINCTRL := $(wildcard ../../panel_pidp1/Pinctrl/*.o)

all: panel_pidp1

panel_pidp1: panel_pidp1.c ../../common.c
	cc $(CFLAGS) -o $@ $^ $(INC) $(LIBS) $(PINCTRL)

clean:
	rm panel_pidp1 >/dev/null 2>&1
