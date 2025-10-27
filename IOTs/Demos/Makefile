files := $(wildcard *.mac)

%.rim:%.mac
	../../bin/macro1_1 $<

all: $(files:.mac=.rim)

clean:
	rm *.rim
	rm *.lst
