# The paper tape reader: rpa (IOT_1.so, an alias) and rpb (IOT_2.so, which serves both).
# IOT_2.so also carries the Type 550 Microtape, which shares device 01 with rpa.
# Its sources# live in ../Microtape and are compiled here.
MT= ../Microtape
INC= -I../../src/blincolnlights -I../../src/blincolnlights/pdp1 -I.. -I../Lib -I$(MT)
MTHDRS= $(MT)/microtape.h $(MT)/control550.h $(MT)/transport555.h
MTOBJS= microtape.o control550.o transport555.o

%.o:%.c ../iotHandler.h
	gcc $(INC) -O3 -fPIC -c $< -o $@

%.so:%.o
	gcc -shared $< -o $@

all: IOT_1.so IOT_2.so

IOT_2.o: IOT_2.c ../iotHandler.h $(MTHDRS)
	gcc $(INC) -O3 -fPIC -c IOT_2.c -o $@

microtape.o: $(MT)/microtape.c $(MTHDRS)
	gcc $(INC) -O3 -fPIC -c $(MT)/microtape.c -o $@

control550.o: $(MT)/control550.c $(MT)/control550.h $(MT)/transport555.h
	gcc $(INC) -O3 -fPIC -c $(MT)/control550.c -o $@

transport555.o: $(MT)/transport555.c $(MT)/transport555.h
	gcc $(INC) -O3 -fPIC -c $(MT)/transport555.c -o $@

IOT_2.so: IOT_2.o $(MTOBJS)
	gcc -shared IOT_2.o $(MTOBJS) -o $@

install: all
	cp *.so ..

clean:
	@rm *.o *.so 2>/dev/null || true
