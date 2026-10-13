# The D/A converters, IOT 71 and IOT 72 (FunStuff/Dacmusic). "make install"
# builds both and copies them to IOTs/, where the emulator loads them; the
# first IOT 71 or 72 a program gives loads them, with no restart. IOT 72 is
# IOT 71's alias for wavetable mode. room.h and edge.h are the room and the
# voicing, shared with FunStuff/Dacmusic/render's dacrender.

INC= -I../../src/blincolnlights -I../../src/blincolnlights/pdp1 -I.. -I../Lib
CFLAGS= -Wall -Wextra -O2 -fPIC -shared
DEPS= ../iotHandler.h ../../src/blincolnlights/pdp1/pdp1.h ../../src/blincolnlights/pdp1/dynamicIots.h \
      ../../src/blincolnlights/pdp1/highSpeedChannels.h ../../src/blincolnlights/pdp1/configuration.h

all: IOT_71.so IOT_72.so

IOT_71.so: IOT_71.c room.h edge.h $(DEPS)
	gcc $(CFLAGS) $(INC) IOT_71.c -o $@ -lm

IOT_72.so: IOT_72.c $(DEPS)
	gcc $(CFLAGS) $(INC) IOT_72.c -o $@

install: IOT_71.so IOT_72.so
	cp IOT_71.so IOT_72.so ..

clean:
	@rm -f *.so

.PHONY: all install clean
