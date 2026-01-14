.PRECIOUS: %.mac

files := $(wildcard *.m1p)

%.mac:%.m1p
	../M1pp/m1pp -o $@ $<

%.rim:%.mac
	/opt/pidp1/bin/macro1_1 $<

all: $(files:.m1p=.rim)

clean:
	@rm *.rim *.lst *.mac 2>/dev/null || true

