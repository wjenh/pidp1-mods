INC=-I..
LIBS=-lpthread -lm
CFLAGS=-O3 -g -Wall

PINCTRL := $(wildcard ../pinctrl/*.o)

panel_pidp1: panel_pidp1.c ../common.c
	cc $(CFLAGS) -o $@ $^ $(INC) $(LIBS) $(PINCTRL)

newpanel: newpanel.c ../common.c
	cc $(CFLAGS) -o $@ $^ $(INC) $(LIBS) $(PINCTRL)

run: panel_pidp1
	./panel_pidp1

runnew: newpanel
	./newpanel

clean:
    rm panel_pidp1 newpanel >/dev/null 2>&1
