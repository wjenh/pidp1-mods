# The paper tape reader: rpa (IOT_1.so, an alias) and rpb (IOT_2.so, which serves both).
# IOT_2.so also carries the Type 550 Microtape, which shares device 01 with rpa.
# Its sources# live in ../Microtape and are compiled here.
MT= ../Microtape
INC= -I../../src/blincolnlights -I../../src/blincolnlights/pdp1 -I.. -I../Lib -I$(MT)
MTHDRS= $(MT)/microtape.h $(MT)/control550.h $(MT)/transport555.h
MTOBJS= microtape.o control550.o transport555.o

%.o:%.c ../iotHandler.h ../../src/blincolnlights/pdp1/dynamicIots.h ../../src/blincolnlights/pdp1/pdp1.h ../../src/blincolnlights/pdp1/ad1intf.h
	gcc $(INC) -O3 -fPIC -c $< -o $@

%.so:%.o
	gcc -shared $< -o $@

all: IOT_1.so IOT_2.so

IOT_2.o: IOT_2.c ../iotHandler.h ../../src/blincolnlights/pdp1/dynamicIots.h ../../src/blincolnlights/pdp1/pdp1.h ../../src/blincolnlights/pdp1/ad1intf.h $(MTHDRS)
	gcc $(INC) -O3 -fPIC -c IOT_2.c -o $@

microtape.o: $(MT)/microtape.c $(MTHDRS) ../iotHandler.h ../../src/blincolnlights/pdp1/dynamicIots.h ../../src/blincolnlights/pdp1/pdp1.h ../../src/blincolnlights/pdp1/ad1intf.h ../../src/blincolnlights/pdp1/configuration.h ../Lib/iotLogger.h ../Lib/writeBehind.h
	gcc $(INC) -O3 -fPIC -c $(MT)/microtape.c -o $@

control550.o: $(MT)/control550.c $(MT)/control550.h $(MT)/transport555.h
	gcc $(INC) -O3 -fPIC -c $(MT)/control550.c -o $@

transport555.o: $(MT)/transport555.c $(MT)/transport555.h
	gcc $(INC) -O3 -fPIC -c $(MT)/transport555.c -o $@

# The microtape writes its image files through the write-behind queue (27-Sep-2026).
IOT_2.so: IOT_2.o $(MTOBJS) ../Lib/writeBehind.o
	gcc -shared IOT_2.o $(MTOBJS) ../Lib/writeBehind.o -lpthread -o $@

install: all
	cp *.so ..

clean:
	@rm *.o *.so 2>/dev/null || true

# Lib's objects are built by its own Makefile, which knows their headers.
../Lib/%.o: FORCE
	$(MAKE) -C ../Lib $*.o

FORCE:
