CC=gcc -O3 -Wno-unused-result
#CC=gcc -g
BISONFLAGS=
#CONFLICTS=-Wcounterexamples

OBJS=ad1.o helpText.o eval.o lex.yy.o y.tab.o listFile.o files.o commands.o \
	flexToAscii.o loader.o ../Disassembler/decode_instruction.o

.PHONY: all clean install
.DELETE_ON_ERROR:

all: ad1

ad1: $(OBJS)
	$(CC) -o ad1 $(OBJS)

clean:
	@rm -f ad1 *.o y.tab.c y.tab.h lex.yy.c parser.output .y.tab.c.old .y.tab.h.old .lex.yy.c.old

install: ad1
	cp ad1 ../../bin
	strip ../../bin/ad1

ad1.o: ad1.c ad1.h pdp1inc.h helpmsgs.h y.tab.h
	$(CC) -c ad1.c

helpText.o: helpText.c
	$(CC) -c helpText.c

files.o: files.c ad1.h
	$(CC) -c files.c

eval.o: eval.c ad1.h y.tab.h
	$(CC) -c eval.c

commands.o: commands.c ad1.h pdp1inc.h helpmsgs.h y.tab.h ../Disassembler/decode_instruction.h
	$(CC) -c commands.c

listFile.o: listFile.c ad1.h
	$(CC) -c listFile.c

loader.o: loader.c ad1.h pdp1inc.h
	$(CC) -c loader.c

flexToAscii.o: flexToAscii.c
	$(CC) -c flexToAscii.c

../Disassembler/decode_instruction.o: ../Disassembler/decode_instruction.c
	(cd ../Disassembler ; make decode_instruction.o)

lex.yy.o: lex.yy.c y.tab.h
	$(CC) -c lex.yy.c

y.tab.o: y.tab.c
	$(CC) -DYYDEBUG=1 -c y.tab.c

# lex.yy.c, y.tab.c and y.tab.h are build artifacts, not checked in.
# They are regenerated on every build rather than only when the source is
# newer, because a git pull over an already-built tree can leave a generated
# file with a newer timestamp than the source it no longer matches, and make
# would then happily keep using it. That produced a scanner whose ':' and ','
# rules were the wrong way round long after lexer.l had been fixed.
# The new output only replaces the old file when the content actually changed,
# so an unchanged generated file keeps its timestamp and nothing recompiles.

lex.yy.c: lexer.l y.tab.h FORCE
	@cp -p lex.yy.c .lex.yy.c.old 2>/dev/null || true
	flex -d lexer.l
	@cmp -s lex.yy.c .lex.yy.c.old && mv -f .lex.yy.c.old lex.yy.c || rm -f .lex.yy.c.old

y.tab.c: parser.y ad1.h pdp1inc.h FORCE
	@cp -p y.tab.c .y.tab.c.old 2>/dev/null || true
	@cp -p y.tab.h .y.tab.h.old 2>/dev/null || true
	bison $(BISONFLAGS) $(CONFLICTS) -o y.tab.c -d parser.y
	@cmp -s y.tab.c .y.tab.c.old && mv -f .y.tab.c.old y.tab.c || rm -f .y.tab.c.old
	@cmp -s y.tab.h .y.tab.h.old && mv -f .y.tab.h.old y.tab.h || rm -f .y.tab.h.old

# bison writes y.tab.h as a side effect of the rule above.
y.tab.h: y.tab.c ;

FORCE:
