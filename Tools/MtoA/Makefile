.PRECIOUS: *.c
CC=gcc -O0 -g

mactoam1: mactoam1.l symtab.o
	flex --outfile mactoam1.c mactoam1.l
	$(CC) -o mactoam1 mactoam1.c symtab.o

symtab.o: symtab.c symtab.h
	$(CC) -c symtab.c

clean:
	@rm mactoam1 mactoam1.c *.o symtabgen 2>/dev/null || true
