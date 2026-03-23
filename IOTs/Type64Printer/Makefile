INC= -I../../src/blincolnlights -I../../src/blincolnlights/pdp1 -I.. -I../Lib
%.o:%.c ../iotHandler.h ../Lib/iotLogger.h ../Lib/flexlib.h
	gcc $(INC) -fPIC -c $< -o $@
%.so:%.o
	gcc -shared $< ../Lib/flextoascii.o ../Lib/filenames.o -o $@

install: IOT_45.so
	cp *.so ..

clean:
	@rm *.o *.so 2>/dev/null || true

