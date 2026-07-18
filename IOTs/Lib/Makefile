INC= -I../../src/blincolnlights -I../../src/blincolnlights/pdp1 -I.. -I../Lib
GCC=gcc $(INC) -O3

install: iotLogger.o asciitoflex.o flextoascii.o filenames.o display.o

iotLogger.o: iotLogger.c iotLogger.h
	$(GCC) -c iotLogger.c

flextoascii.o: flextoascii.c flexlib.h
	$(GCC) -c flextoascii.c

asciitoflex.o: asciitoflex.c flexlib.h
	$(GCC) -c asciitoflex.c

filenames.o: filenames.c
	$(GCC) -c filenames.c

display.o: display.c
	$(GCC) -c display.c

clean:
	@rm *.o 2>/dev/null || true
