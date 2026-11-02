# SHRDLU as a native Linux program: "make" builds shrdlu here, needing gcc and nothing
# else. code.c and image.c are generated from the Lisp and kept here, so the build does not
# need the Lisp.

CC = gcc
CFLAGS = -std=gnu17 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-clobbered
OBJS = rt.o top.o scene.o term.o main.o image.o code.o

shrdlu: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -lpthread -o $@

install: shrdlu
	cp shrdlu ..

rt.o: rt.c rt.h
top.o: top.c top.h rt.h
scene.o: scene.c scene.h
term.o: term.c term.h rt.h scene.h top.h
main.o: main.c rt.h scene.h term.h top.h

# The tables gain nothing from optimizing and take much longer at -O2.
image.o: image.c rt.h
	$(CC) -std=gnu17 -O1 -w -c image.c -o $@

# The generated code is correct as it stands, and draws many warnings, so they are off.
code.o: code.c rt.h
	$(CC) -std=gnu17 -O2 -g -w -c code.c -o $@

clean:
	rm -f shrdlu $(OBJS)

.PHONY: clean
