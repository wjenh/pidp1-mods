INC= -I../src/blincolnlights -I../src/blincolnlights/pdp1
files := $(wildcard *.c)

%.so:%.o
	gcc -shared $< Logger/iotLogger.o -o $@
%.o:%.c iotHandler.h
	gcc $(INC) -fPIC -c $< -o $@

all: $(files:.c=.so) Logger/iotLogger.o

Logger/iotLogger.o: Logger/iotLogger.c Logger/iotLogger.h
	(cd Logger; make)

clean:
	rm *.so
