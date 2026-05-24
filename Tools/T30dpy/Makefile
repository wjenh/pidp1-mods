#GCC=gcc -O3 -Wno-unused-result
GCC=gcc -g -Wno-unused-result

t30dpy:	t30dpy.o
	$(GCC) $^ -o t30dpy -lSDL2 -lSDL2main -lm

t30dpy.o: t30dpy.c
	$(GCC) -c t30dpy.c

dpytester:	dpytester.o
	$(GCC) $^ -o dpytester

dpytester.o: dpytester.c
	$(GCC) -c dpytester.c

clean:
	@rm *.o t30dpy dpytester 2>/dev/null || true

install: t30dpy
	cp t30dpy ../../bin
	strip ../../bin/t30dpy

