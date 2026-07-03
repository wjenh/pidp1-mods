INC= -I../../src/blincolnlights -I../../src/blincolnlights/pdp1 -I..

%.o:%.c ../iotHandler.h
	gcc $(INC) -O3 -fPIC -c $< -o $@

%.so:%.o
	gcc -shared $< -o $@

all: IOT_5.so IOT_6.so

install: all
	cp *.so ..

clean:
	@rm *.o *.so 2>/dev/null || true
