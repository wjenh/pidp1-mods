# Type 550 Microtape.
# The tape has no plugin of its own, it shares device 01 with the paper
# tape reader,  ../Reader/Makefile compiles microtape.c, control550.c and transport555.c from
# here into IOT_2.so.
# See README.txt. "make install" only builds the
# image tool mkmicrotape here, it installs nothing into the IOTs directory.
#
install: mkmicrotape
	strip mkmicrotape
	cp mkmicrotape ../../bin

mkmicrotape: mkmicrotape.c transport555.c transport555.h
	gcc -O2 -Wall -o $@ mkmicrotape.c transport555.c

clean:
	@rm -f *.o *.so mkmicrotape 2>/dev/null || true
