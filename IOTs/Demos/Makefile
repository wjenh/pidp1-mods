files := $(wildcard *.mac)

%.rim:%.mac
	../../bin/macro1_1 $<

install:
	echo "Demos are not automatically built, do so manually"

all: $(files:.mac=.rim)

clean:
	@rm *.rim *.lst 2>/dev/null || true
