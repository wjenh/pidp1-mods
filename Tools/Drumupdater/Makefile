all: drumupdater drumlist 

drumupdater: drumupdater.o

drumlist: drumlist.c
	gcc -O0 -g drumlist.c -o drumlist

clean:
	@rm *.o drumupdater drumlist 2>/dev/null || true
