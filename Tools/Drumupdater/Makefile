CC=gcc

all: drumupdater drumlist 

drumupdater: drumupdater.c
	$(CC) drumupdater.c -o drumupdater

drumlist: drumlist.c
	$(CC) drumlist.c -o drumlist

clean:
	@rm *.o drumupdater drumlist 2>/dev/null || true
