GCC=gcc -O3 -march=native -Wall -Wextra -Wno-unused-result -Wno-implicit-fallthrough -Wno-switch \
    -Wno-unused-parameter
#GCC=gcc -g -Og -march=native -Wall -Wextra -Wno-unused-result -Wno-implicit-fallthrough -Wno-switch \
    -fsanitize=address,undefined

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
