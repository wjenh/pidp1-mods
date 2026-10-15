CC=gcc -O0 -g
//CC=gcc

# drumlist compiles ovlload's .ovl reader straight in, as ovlload does, so that no
# object file is left in either directory.
OVLDIR=../Ovlload

all: drumupdater drumlist 

install: drumupdater drumlist
	cp drumupdater ../../bin
	cp drumlist ../../bin
	strip ../../bin/drumupdater
	strip ../../bin/drumlist

drumupdater: drumupdater.c
	$(CC) drumupdater.c -o drumupdater

drumlist: drumlist.c $(OVLDIR)/ovlfile.c $(OVLDIR)/ovlfile.h
	$(CC) -I$(OVLDIR) drumlist.c $(OVLDIR)/ovlfile.c -o drumlist

clean:
	@rm *.o drumupdater drumlist 2>/dev/null || true
