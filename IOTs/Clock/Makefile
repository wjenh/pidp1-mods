INC= -I../../src/blincolnlights -I../../src/blincolnlights/pdp1 -I.. -I../Lib
%.o:%.c ../iotHandler.h ../Lib/iotLogger.h
	gcc $(INC) -fPIC -c $< -o $@
%.so:%.o
	gcc -shared $< -o $@

install: IOT_32.so
	cp *.so ..

clean:
	@rm *.o *.so 2>/dev/null || true
