INC= -I../../src/blincolnlights -I../../src/blincolnlights/pdp1 -I.. -I../Lib
%.o:%.c ../iotHandler.h ../../src/blincolnlights/pdp1/dynamicIots.h ../../src/blincolnlights/pdp1/pdp1.h ../../src/blincolnlights/pdp1/ad1intf.h ../Lib/iotLogger.h
	gcc $(INC) -O3 -fPIC -c $< -o $@
%.so:%.o ../Lib/iotLogger.o
	gcc -shared $< ../Lib/iotLogger.o -o $@

# IOT_61 writes the drum file through the write-behind queue (27-Sep-2026).
IOT_61.o: IOT_61.c ../iotHandler.h ../../src/blincolnlights/pdp1/dynamicIots.h ../../src/blincolnlights/pdp1/pdp1.h ../../src/blincolnlights/pdp1/ad1intf.h ../Lib/iotLogger.h ../Lib/writeBehind.h ../../src/blincolnlights/pdp1/highSpeedChannels.h
	gcc $(INC) -O3 -fPIC -c $< -o $@

IOT_61.so: IOT_61.o ../Lib/iotLogger.o ../Lib/writeBehind.o
	gcc -shared $< ../Lib/iotLogger.o ../Lib/writeBehind.o -lpthread -o $@

install: IOT_61.so IOT_62.so IOT_63.so
	cp *.so ..

progs: drumloader.rim drumdiag.rim drumtest.rim

drumloader.rim: drumloader.mac
	../../bin/macro1_1 -r drumloader.mac

drumtest.rim: drumtest.mac
	../../bin/macro1_1 drumtest.mac

drumdiag.rim: drumdiag.mac
	../../bin/macro1_1 drumdiag.mac

clean:
	@rm *.o *.so *.rim *.lst 2>/dev/null || true

# Lib's objects are built by its own Makefile, which knows their headers.
../Lib/%.o: FORCE
	$(MAKE) -C ../Lib $*.o

FORCE:
