INC= -I../../src/blincolnlights -I../../src/blincolnlights/pdp1 -I.. -I../Lib
%.o:%.c ../iotHandler.h ../../src/blincolnlights/pdp1/dynamicIots.h ../../src/blincolnlights/pdp1/pdp1.h ../../src/blincolnlights/pdp1/ad1intf.h ../Lib/iotLogger.h
	gcc $(INC) -O3 -fPIC -c $< -o $@
%.so:%.o ../Lib/iotLogger.o
	gcc -shared $< ../Lib/iotLogger.o -o $@

files := $(wildcard *.mac)
cfiles := $(wildcard *.c)

%.rim:%.mac
	../../bin/macro1_1 $<

install:
	echo "Demos are not automatically built, do so manually via make all"

all: $(files:.mac=.rim) iots

iots: $(cfiles:.c=.so)

clean:
	@rm *.rim *.lst *.so 2>/dev/null || true

# Lib's objects are built by its own Makefile, which knows their headers.
../Lib/%.o: FORCE
	$(MAKE) -C ../Lib $*.o

FORCE:
