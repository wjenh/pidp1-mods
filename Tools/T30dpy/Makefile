#GCC=gcc -O3 -march=native -Wno-unused-result
GCC=gcc -g -march=native -Wno-unused-result

all: t30dpy t30dpy3

t30dpy:	t30dpy.o
	$(GCC) $^ -o t30dpy -lSDL2 -lSDL2main -lm

t30dpy.o: t30dpy.c
	$(GCC) -c t30dpy.c

t30dpy3:	t30dpy3.o
	$(GCC) $^ -o t30dpy3 -lSDL3 -lm

t30dpy3.o: t30dpy3.c
	$(GCC) -c t30dpy3.c

dpytester:	dpytester.o
	$(GCC) $^ -o dpytester

dpytester.o: dpytester.c
	$(GCC) -c dpytester.c

clean:
	@rm *.o t30dpy t30dpy3 dpytester 2>/dev/null || true

install: t30dpy
	cp t30dpy ../../bin
	strip ../../bin/t30dpy

installdpy3: t30dpy3
	cp t30dpy3 ../../bin/t30dpy
	strip ../../bin/t30dpy

t30new:	t30new.o
	$(GCC) $^ -o t30new -lSDL3 -lm

t30new.o: t30new.c
	$(GCC) -c t30new.c

