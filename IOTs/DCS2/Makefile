INC= -I../../src/common -I../../src/pdp1 -I.. -I../Lib
%.o:%.c ../iotHandler.h ../../src/pdp1/dynamicIots.h ../../src/pdp1/pdp1.h ../../src/pdp1/ad1intf.h ../Lib/iotLogger.h ../Lib/flexlib.h
	gcc $(INC) -O3 -fPIC -c $< -o $@
%.so:%.o ../Lib/iotLogger.o ../Lib/flextoascii.o ../Lib/asciitoflex.o
	gcc -shared $< ../Lib/iotLogger.o ../Lib/flextoascii.o ../Lib/asciitoflex.o -o $@

install: IOT_22.so
	cp *.so ..

clean:
	@rm *.o *.so 2>/dev/null || true

# Lib's objects are built by its own Makefile, which knows their headers.
../Lib/%.o: FORCE
	$(MAKE) -C ../Lib $*.o

FORCE:
