INC=-I.. -I../pdp1
LIBS=-lpthread -lm
CFLAGS= $(INC) -O3 -Wno-unused-result

PINCTRL := $(wildcard ../pinctrl/*.o)

all: newpanel

newpanel: newpanel.c ../common.c ../pdp1/configuration.c
	cc $(CFLAGS) -o $@ $^ $(INC) $(LIBS) $(PINCTRL)

run: newpanel
	./newpanel

clean:
	rm  newpanel >/dev/null 2>&1
