CC=gcc -O1 -g -Wall

all: mactoam1

install: mactoam1
	cp mactoam1 ../../bin/mactoam1
	strip ../../bin/mactoam1

mactoam1: mactoam1.o m1engine.o
	$(CC) -o mactoam1 mactoam1.o m1engine.o

mactoam1.o: mactoam1.c m1engine.h
	$(CC) -c mactoam1.c

m1engine.o: m1engine.c m1engine.h
	$(CC) -c m1engine.c

clean:
	@rm mactoam1 *.o 2>/dev/null || true
