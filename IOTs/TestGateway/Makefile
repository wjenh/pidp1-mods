INC= -I../../src/blincolnlights -I../../src/blincolnlights/pdp1 -I.. -I../Lib
%.o:%.c ../iotHandler.h ../Lib/iotLogger.h
	gcc $(INC) -fPIC -c $< -o $@
%.so:%.o
	gcc -shared $< ../Lib/iotLogger.o -o $@

install: IOT_44.so
	cp *.so ..

clean:
	@rm *.o *.so 2>/dev/null || true
