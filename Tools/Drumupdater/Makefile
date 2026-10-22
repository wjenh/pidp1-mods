CC=gcc -O0 -g
//CC=gcc

# drumlist compiles ovlload's .ovl reader straight in, as ovlload does, so that no
# object file is left in either directory.
UTILLIBS=../UtilLibs

all: drumupdater drumlist 

install: drumupdater drumlist
	cp drumupdater ../../bin
	cp drumlist ../../bin
	strip ../../bin/drumupdater
	strip ../../bin/drumlist

drumupdater: drumupdater.c
	$(CC) drumupdater.c -o drumupdater

drumlist: drumlist.c $(UTILLIBS)/ovlfile.c $(UTILLIBS)/ovlfile.h
	$(CC) -I$(UTILLIBS) drumlist.c $(UTILLIBS)/ovlfile.c -o drumlist

clean:
	@rm *.o drumupdater drumlist 2>/dev/null || true
