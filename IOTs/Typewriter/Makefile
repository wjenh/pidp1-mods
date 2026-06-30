INC= -I../../src/blincolnlights -I../../src/blincolnlights/pdp1 -I.. -I../Lib
%.o:%.c ../iotHandler.h ../Lib/iotLogger.h
	gcc $(INC) -fPIC -c $< -o $@
%.so:%.o
	gcc -shared $< ../Lib/iotLogger.o -o $@

all: IOT_3.so IOT_4.so

install: all
	cp *.so ..

clean:
	@rm *.o *.so 2>/dev/null || true
