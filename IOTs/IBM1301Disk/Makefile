# The IBM 1301 disk: IOT_10, with IOT_36 and IOT_46 its aliases.
# ROOT is the pidp1-mods directory.
ROOT ?= ../..
INC= -I. -I$(ROOT)/src/common -I$(ROOT)/src/pdp1 -I$(ROOT)/IOTs -I$(ROOT)/IOTs/Lib
HDRS= $(ROOT)/IOTs/iotHandler.h $(ROOT)/src/pdp1/dynamicIots.h $(ROOT)/src/pdp1/pdp1.h \
    $(ROOT)/src/pdp1/ad1intf.h $(ROOT)/IOTs/Lib/iotLogger.h

all: IOT_10.so IOT_36.so IOT_46.so

IOT_10.o: IOT_10.c ibm1301image.h $(HDRS) $(ROOT)/src/pdp1/highSpeedChannels.h $(ROOT)/src/pdp1/configuration.h
	gcc $(INC) -O3 -Wall -fPIC -c $< -o $@

%.o: %.c $(HDRS)
	gcc $(INC) -O3 -Wall -fPIC -c $< -o $@

# The logger is compiled here, so a build never writes into IOTs/Lib.
iotLogger.o: $(ROOT)/IOTs/Lib/iotLogger.c $(ROOT)/IOTs/Lib/iotLogger.h
	gcc $(INC) -O3 -fPIC -c $< -o $@

%.so: %.o iotLogger.o
	gcc -shared $< iotLogger.o -o $@

install: all
	cp IOT_10.so IOT_36.so IOT_46.so ..

clean:
	@rm -f *.o *.so
