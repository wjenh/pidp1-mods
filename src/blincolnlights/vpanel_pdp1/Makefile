# Every file main.c and common.c include, from gcc -MM -I.. main.c ../common.c.
DEPS = elements.inc ../art/panelart.inc ../art/pdp1art.inc ../common.h ../panel_pidp1.h

panel_pdp1: main.c ../common.c $(DEPS)
	$(CC) -O3 -I.. -o $@ main.c ../common.c -lSDL2 -lSDL2_image
