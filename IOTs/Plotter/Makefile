INC= -I../../src/common -I../../src/pdp1 -I.. -I../Lib
CFLAGS= -O3
#CFLAGS= -g
HDRS= ../iotHandler.h ../../src/pdp1/dynamicIots.h ../../src/pdp1/pdp1.h ../../src/pdp1/ad1intf.h ../../src/pdp1/configuration.h ../Lib/iotLogger.h ../Lib/writeBehind.h pngWrite.h

all: IOT_73.so

IOT_73.o: IOT_73.c $(HDRS)
	gcc $(INC) $(CFLAGS) -fPIC -c $< -o $@

pngWrite.o: pngWrite.c pngWrite.h
	gcc $(CFLAGS) -fPIC -c $< -o $@

IOT_73.so: IOT_73.o pngWrite.o ../Lib/iotLogger.o ../Lib/writeBehind.o
	gcc -shared IOT_73.o pngWrite.o ../Lib/iotLogger.o ../Lib/writeBehind.o -lz -lpthread -lm -o $@

install: all
	cp IOT_73.so ..

clean:
	@rm *.o *.so 2>/dev/null || true
