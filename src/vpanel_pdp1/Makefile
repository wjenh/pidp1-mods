# Every file main.c, common.c and configuration.c include, from gcc -MM -I../common -I../pdp1 main.c ../common/common.c.
DEPS = elements.inc art/panelart.inc art/pdp1art.inc art/ioart.inc ../common/common.h ../common/panel_pidp1.h \
	../pdp1/configuration.h

panel_pdp1: main.c ../common/common.c ../pdp1/configuration.c $(DEPS)
	$(CC) -O3 -I../common -I../pdp1 -o $@ main.c ../common/common.c ../pdp1/configuration.c -lSDL2 -lSDL2_image -lm
