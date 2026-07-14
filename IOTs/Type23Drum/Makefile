INC= -I../../src/blincolnlights -I../../src/blincolnlights/pdp1 -I.. -I../Lib
%.o:%.c ../iotHandler.h ../Lib/iotLogger.h
	gcc $(INC) -O3 -fPIC -c $< -o $@
%.so:%.o
	gcc -shared $< ../Lib/iotLogger.o -o $@

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
