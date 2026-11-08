INC= -I../src/common -I../src/pdp1 -ILib
files := $(wildcard *.c)

%.so:%.o
	gcc -shared $< -o $@
%.o:%.c iotHandler.h ../src/pdp1/dynamicIots.h ../src/pdp1/pdp1.h ../src/pdp1/ad1intf.h
	gcc $(INC) -fPIC -c $< -o $@

all: dist $(files:.c=.so)

dist:
	./install.sh
clean:
	@rm *.o *.so 2>/dev/null || true
	./cleanall.sh

