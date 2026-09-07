CC = gcc
CFLAGS = -g -Wall
YACC = bison
LEX = flex
BISONFLAGS =
#CONFLICTS=-Wcounterexamples

all: advdataloader advdrumloader advsave

install: advdataloader advdrumloader advsave
	strip advdataloader
	strip advdrumloader
	strip advsave
	cp advdrumloader ../advdrumloader
	cp advsave ../advsave

advdataloader: advdataloader.o symtab.o lex.yy.o y.tab.o
	$(CC) $(CFLAGS) -o advdataloader advdataloader.o symtab.o lex.yy.o y.tab.o

advdrumloader: advdrumloader.o
	$(CC) $(CFLAGS) -o advdrumloader advdrumloader.o

advsave: advsave.o
	$(CC) $(CFLAGS) -o advsave advsave.o

clean:
	@rm -f advdataloader advdrumloader advsave *.o y.tab.c y.tab.h lex.yy.c 2>/dev/null || true

advdataloader.o: advdataloader.c advdataloader.h symtab.h y.tab.h
	$(CC) $(CFLAGS) -c advdataloader.c

# advmagic.h holds SAVE_MAGIC and WIZCOM_MAGIC and is listed here on
# purpose: it was missing until 05-Sep-26, so bumping SAVE_MAGIC left
# these two objects stale and `make` said "Nothing to be done". A stale
# advdrumloader stops recognising the SAVE block it is meant to preserve
# and zeroes every save -- exactly the failure advmagic.h's own header
# comment describes, and exactly what happened again when TASK-FR31 N41
# bumped the value to 0x7A72.
advdrumloader.o: advdrumloader.c advdataloader.h advmagic.h
	$(CC) $(CFLAGS) -c advdrumloader.c

advsave.o: advsave.c advdataloader.h advmagic.h
	$(CC) $(CFLAGS) -c advsave.c

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
