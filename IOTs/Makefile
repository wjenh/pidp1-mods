INC= -I../src/blincolnlights -I../src/blincolnlights/pdp1 -ILib
files := $(wildcard *.c)

%.so:%.o
	gcc -shared $< -o $@
%.o:%.c iotHandler.h
	gcc $(INC) -fPIC -c $< -o $@

all: dist $(files:.c=.so)

dist:
	./install.sh
clean:
	@rm *.o *.so 2>/dev/null || true
	./cleanall.sh

