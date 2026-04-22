%.rim: %.am1
	am1 -l $<

AM1S = $(wildcard *.am1)
RIMS = $(AM1S:.am1=.rim)

all: $(RIMS)

install:
	echo These are tests of the Type 340 Display and are not installed.

clean:
	@rm *.rim *.lst 2>/dev/null || true
