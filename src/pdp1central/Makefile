# pdp1central, the desktop control app for the PiDP-1 emulator.
# Needs SDL2 2.0.18 or later (the Nuklear renderer uses SDL_RenderGeometry).
# Header dependencies are listed by hand from gcc -MM; a new #include updates them here.

CC=gcc
# Every unit that includes nuklear.h must see the same NK_INCLUDE_* options.
NKFLAGS=-DNK_INCLUDE_FIXED_TYPES -DNK_INCLUDE_STANDARD_IO -DNK_INCLUDE_STANDARD_VARARGS \
	-DNK_INCLUDE_DEFAULT_ALLOCATOR -DNK_INCLUDE_VERTEX_BUFFER_OUTPUT -DNK_INCLUDE_FONT_BAKING \
	-DNK_INCLUDE_DEFAULT_FONT
CFLAGS=-O2 -g -Wall -Wextra -D_GNU_SOURCE `sdl2-config --cflags`
LIBS=`sdl2-config --libs` -lm

CORE=procs.o cmdport.o child.o conffile.o schema.o mtlist.o

all: pdp1central

pdp1central: pdp1central.o style.o nk.o $(CORE)
	$(CC) -o $@ $^ $(LIBS)

pdp1central.o: pdp1central.c core.h style.h nuklear.h nuklear_sdl_renderer.h
	$(CC) $(CFLAGS) $(NKFLAGS) -c -o $@ pdp1central.c

style.o: style.c style.h core.h nuklear.h
	$(CC) $(CFLAGS) $(NKFLAGS) -c -o $@ style.c

# Nuklear's own warnings are not ours to fix; it is vendored unmodified.
nk.o: nk.c nuklear.h nuklear_sdl_renderer.h
	$(CC) $(CFLAGS) $(NKFLAGS) -w -c -o $@ nk.c

procs.o: procs.c core.h
	$(CC) $(CFLAGS) -c -o $@ procs.c

cmdport.o: cmdport.c core.h
	$(CC) $(CFLAGS) -c -o $@ cmdport.c

child.o: child.c core.h
	$(CC) $(CFLAGS) -c -o $@ child.c

conffile.o: conffile.c core.h
	$(CC) $(CFLAGS) -c -o $@ conffile.c

schema.o: schema.c core.h
	$(CC) $(CFLAGS) -c -o $@ schema.c

mtlist.o: mtlist.c core.h
	$(CC) $(CFLAGS) -c -o $@ mtlist.c

clean:
	rm -f pdp1central *.o

.PHONY: all clean
