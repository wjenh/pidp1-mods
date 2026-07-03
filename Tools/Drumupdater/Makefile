CC=gcc -O3
//CC=gcc

all: drumupdater drumlist 

install: drumupdater drumlist
	cp drumupdater ../../bin
	cp drumlist ../../bin
	strip ../../bin/drumupdater
	strip ../../bin/drumlist

drumupdater: drumupdater.c
	$(CC) drumupdater.c -o drumupdater

drumlist: drumlist.c
	$(CC) drumlist.c -o drumlist

clean:
	@rm *.o drumupdater drumlist 2>/dev/null || true
