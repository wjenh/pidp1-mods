INC=-I../.. -I../../pdp1 -I../Pinctrl
LIBS=-lpthread -lm
CFLAGS= $(INC) -O3 -Wno-unused-result

PINCTRL := $(wildcard ../Pinctrl/*.o)

all: panel_pidp1

panel_pidp1: panel_pidp1.c ../../common.c
	cc $(CFLAGS) -o $@ $^ $(INC) $(LIBS) $(PINCTRL)

clean:
	rm panel_pidp1 >/dev/null 2>&1
