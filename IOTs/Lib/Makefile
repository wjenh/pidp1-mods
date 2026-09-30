INC= -I../../src/blincolnlights -I../../src/blincolnlights/pdp1 -I.. -I../Lib
GCC=gcc $(INC) -O3 -fPIC

install: iotLogger.o asciitoflex.o flextoascii.o filenames.o display.o writeBehind.o

iotLogger.o: iotLogger.c iotLogger.h
	$(GCC) -c iotLogger.c

flextoascii.o: flextoascii.c flexlib.h
	$(GCC) -c flextoascii.c

asciitoflex.o: asciitoflex.c flexlib.h
	$(GCC) -c asciitoflex.c

filenames.o: filenames.c ../../src/blincolnlights/pdp1/pdp1.h ../../src/blincolnlights/pdp1/ad1intf.h
	$(GCC) -c filenames.c

display.o: display.c
	$(GCC) -c display.c

# The drum's and the microtape's write-behind queue (27-Sep-2026).
writeBehind.o: writeBehind.c writeBehind.h
	$(GCC) -c writeBehind.c

clean:
	@rm *.o 2>/dev/null || true
