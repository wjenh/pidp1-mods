INC=-I.. -I../pdp1
LIBS=-lpthread -lm
CFLAGS= $(INC) -O3 -Wno-unused-result

PINCTRL := $(wildcard ../pinctrl/*.o)

all: panel_pidp1 newpanel

panel_pidp1: panel_pidp1.c ../common.c
	cc $(CFLAGS) -o $@ $^ $(INC) $(LIBS) $(PINCTRL)

newpanel: newpanel.c ../common.c ../pdp1/configuration.c
	cc $(CFLAGS) -o $@ $^ $(INC) $(LIBS) $(PINCTRL)

run: panel_pidp1
	./panel_pidp1

runnew: newpanel
	./newpanel

clean:
	rm panel_pidp1 newpanel >/dev/null 2>&1
