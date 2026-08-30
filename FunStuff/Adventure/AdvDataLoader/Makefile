CC = gcc
CFLAGS = -g -Wall
YACC = bison
LEX = flex
BISONFLAGS =
#CONFLICTS=-Wcounterexamples

all: advdataloader advdrumloader

install: advdataloader advdrumloader
	cp advdataloader ../advdataloader
	cp advdrumloader ../advdrumloader
	strip ../advdataloader
	strip ../advdrumloader

advdataloader: advdataloader.o symtab.o lex.yy.o y.tab.o
	$(CC) $(CFLAGS) -o advdataloader advdataloader.o symtab.o lex.yy.o y.tab.o

advdrumloader: advdrumloader.o
	$(CC) $(CFLAGS) -o advdrumloader advdrumloader.o

clean:
	@rm -f advdataloader advdrumloader *.o y.tab.c y.tab.h lex.yy.c 2>/dev/null || true

advdataloader.o: advdataloader.c advdataloader.h symtab.h y.tab.h
	$(CC) $(CFLAGS) -c advdataloader.c

advdrumloader.o: advdrumloader.c
	$(CC) $(CFLAGS) -c advdrumloader.c

symtab.o: symtab.c symtab.h
	$(CC) $(CFLAGS) -c symtab.c

lex.yy.o: lex.yy.c y.tab.h symtab.h advdataloader.h
	$(CC) $(CFLAGS) -c lex.yy.c

lex.yy.c: lexer.l
	$(LEX) lexer.l

y.tab.c y.tab.h: parser.y advdataloader.h symtab.h
	$(YACC) $(BISONFLAGS) $(CONFLICTS) -o y.tab.c -d parser.y

y.tab.o: y.tab.c
	$(CC) $(CFLAGS) -DYYDEBUG=1 -c y.tab.c
