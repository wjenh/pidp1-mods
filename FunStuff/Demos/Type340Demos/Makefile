%.rim:%.am1
	am1 $<

SOURCES := $(wildcard *.am1)
RIMS := $(patsubst %.am1,%.rim,$(SOURCES))

all: $(RIMS)

clean:
	@rm *.rim 2>/dev/null || true

