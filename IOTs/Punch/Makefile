INC= -I../../src/common -I../../src/pdp1 -I.. -I../Lib
CFLAGS= -O3
#CFLAGS= -g

%.o:%.c ../iotHandler.h ../../src/pdp1/dynamicIots.h ../../src/pdp1/pdp1.h ../../src/pdp1/ad1intf.h ../../src/pdp1/configuration.h
	gcc $(INC) $(CFLAGS) -fPIC -c $< -o $@

%.so:%.o
	gcc -shared $< -o $@

all: IOT_5.so IOT_6.so

install: all
	cp *.so ..

clean:
	@rm *.o *.so 2>/dev/null || true
