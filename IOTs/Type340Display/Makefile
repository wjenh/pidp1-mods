INC= -I../../src/blincolnlights -I../../src/blincolnlights/pdp1 -I.. -I../Lib
CFLAGS= -O3
#CFLAGS= -g

%.o:%.c ../iotHandler.h ../Lib/iotLogger.h type340emu.h
	gcc $(INC) $(CFLAGS) -fPIC -c $< -o $@

%.so:%.o type340emu.o
	gcc -shared $< ../Lib/iotLogger.o type340emu.o -o $@

all: IOT_15.so IOT_16.so IOT_17.so

install: all
	cp *.so ..

IOT_15.o: IOT_15.c type340emu.h type340emu.o
	gcc $(INC) $(CFLAGS) -fPIC -c IOT_15.c

IOT_16.o: IOT_16.c
	gcc $(INC) $(CFLAGS) -fPIC -c IOT_16.c

IOT_17.o: IOT_17.c
	gcc $(INC) $(CFLAGS) -fPIC -c IOT_17.c

type340emu.o: type340emu.c type340emu.h
	gcc $(INC) $(CFLAGS) -fPIC -c type340emu.c

clean:
	@rm *.o *.so 2>/dev/null || true
