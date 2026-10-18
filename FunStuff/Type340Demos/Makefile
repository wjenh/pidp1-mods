%.rim:%.am1
	am1 $<

SOURCES := $(wildcard *.am1)
RIMS := $(patsubst %.am1,%.rim,$(SOURCES))

all: $(RIMS)

clean:
	@rm *.rim *.ovl 2>/dev/null || true

