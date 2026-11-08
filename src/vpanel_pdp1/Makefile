# Every file main.c and common.c include, from gcc -MM -I../common main.c ../common/common.c.
DEPS = elements.inc art/panelart.inc art/pdp1art.inc ../common/common.h ../common/panel_pidp1.h

panel_pdp1: main.c ../common/common.c $(DEPS)
	$(CC) -O3 -I../common -o $@ main.c ../common/common.c -lSDL2 -lSDL2_image
