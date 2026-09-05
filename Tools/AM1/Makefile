CC=gcc $(CCFLAGS)
#CCFLAGS= -O3 -s
CCFLAGS= -g
#BISONFLAGS=-Wcounterexamples
BISONFLAGS=

OBJS=am1.o permsyms.def.o symtab.o lex.yy.o y.tab.o parsefns.o eval.o \
	maccodegen.o bincodegen.o listcodegen.o testcodegen.o listSymtab.o

.PHONY: all clean install
.DELETE_ON_ERROR:

all: am1 fastload

am1:	$(OBJS)
	$(CC) -o am1 $^

fastload: fastload.c
	$(CC) fastload.c -o fastload

clean:
	@rm -f *.o lex.yy.c y.tab.c y.tab.h permsyms.def.c am1 fastload symtabgen cvtxldr \
	xldr.rim xldr.lst parser.output .y.tab.c.old .y.tab.h.old .lex.yy.c.old .permsyms.def.c.old

install: am1 fastload
	cp am1 ../../bin
	cp fastload ../../bin
	strip ../../bin/am1
	strip ../../bin/fastload

permsyms.def.o: permsyms.def.c
	$(CC) -c permsyms.def.c

# lexfuncs.c is #included by lexer.l into the third section of the scanner,
# so it is compiled as part of lex.yy.c rather than appearing in it. It has to
# be a dependency of the object, not just of the generated source.
lex.yy.o: lex.yy.c y.tab.h lexfuncs.c
	$(CC) -c lex.yy.c

y.tab.o: y.tab.c
	$(CC) -DYYDEBUG=1 -c y.tab.c

permsyms.def.c: symtabgen permsyms.def
	@cp -p permsyms.def.c .permsyms.def.c.old 2>/dev/null || true
	./symtabgen permsyms.def
	@cmp -s permsyms.def.c .permsyms.def.c.old && mv -f .permsyms.def.c.old permsyms.def.c \
	|| rm -f .permsyms.def.c.old

lex.yy.c: lexer.l y.tab.h FORCE
	flex -d lexer.l

%.tab.c %.tab.h: parser.y am1.h symtab.h FORCE
	bison $(BISONFLAGS) -o y.tab.c -d parser.y

am1.o: am1.c am1.h symtab.h y.tab.h
	$(CC) -c am1.c

parsefns.o: parsefns.c am1.h y.tab.h
	$(CC) -c parsefns.c

eval.o: eval.c am1.h y.tab.h
	$(CC) -c eval.c

maccodegen.o: maccodegen.c am1.h y.tab.h
	$(CC) -c maccodegen.c

bincodegen.o: bincodegen.c am1.h y.tab.h font5x7.h xldr.h
	$(CC) -c bincodegen.c

listcodegen.o: listcodegen.c am1.h y.tab.h
	$(CC) -c listcodegen.c

testcodegen.o: testcodegen.c am1.h y.tab.h
	$(CC) -c testcodegen.c

listSymtab.o: listSymtab.c am1.h symtab.h
	$(CC) -c listSymtab.c

symtabgen: symtabgen.c am1.h symtab.o
	$(CC) -o symtabgen symtabgen.c symtab.o

symtab.o: symtab.c symtab.h
	$(CC) -c symtab.c

cvtxldr: cvtxldr.c
	$(CC) -o cvtxldr cvtxldr.c

xldr: cvtxldr
	$(MACRO) xldr.src
	cvtxldr xldr.lst >xldr.h

FORCE:
