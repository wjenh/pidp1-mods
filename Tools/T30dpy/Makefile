GCC=gcc -O3 -Wno-unused-result
#GCC=gcc -g -Wno-unused-result

t30dpy:	t30dpy.o
	$(GCC) $^ -o t30dpy -lSDL2 -lSDL2main -lm

clean:
	@rm *.o t30dpy 2>/dev/null || true

t30dpy.o: t30dpy.c
	$(GCC) -c t30dpy.c

install: t30dpy
	cp t30dpy ../../bin
	strip ../../bin/t30dpy

