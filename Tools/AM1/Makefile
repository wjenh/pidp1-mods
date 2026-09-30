CC=gcc $(CCFLAGS)
CCFLAGS= -O3 -s
#CCFLAGS= -g
#BISONFLAGS=-Wcounterexamples
BISONFLAGS=
# LINKDIR is where fastload finds the socket info for connecting to the pdp-1
LINKDIR=../AD1

OBJS=am1.o permsyms.def.o symtab.o lex.yy.o y.tab.o parsefns.o eval.o \
	maccodegen.o bincodegen.o listcodegen.o testcodegen.o listSymtab.o optimizer.o \
	optdecode.o optrefs.o optflow.o optreturn.o optroutine.o optcallgraph.o \
	optstrings.o optrules.o optreport.o optshare.o optregion.o opttransform.o \
	optvalue.o optscratch.o optguess.o optspeed.o optrelayout.o optwindow.o \
	srccodegen.o

# am1test is am1 built with AM1_TEST_SWITCHES defined, which also accepts the
# -O= modifiers that exist to test the optimizer; wordtable_check.sh and
# Tests/Symbols run it.  Only am1.c and optimizer.c test the symbol, so only
# they are compiled twice.  am1test is never installed.
TESTOBJS=$(filter-out am1.o optimizer.o,$(OBJS)) am1-test.o optimizer-test.o

all: am1 fastload

am1:	$(OBJS)
	$(CC) -o am1 $^

am1test: $(TESTOBJS)
	$(CC) -o am1test $^

# fastload uses ad1's debugger link client, compiled straight in so that no
# object file is left in $(LINKDIR).
fastload: fastload.c $(LINKDIR)/ad1link.c $(LINKDIR)/ad1link.h $(LINKDIR)/ad1proto.h
	$(CC) -I$(LINKDIR) fastload.c $(LINKDIR)/ad1link.c -o fastload

clean:
	@rm -f *.o lex.yy.c y.tab.c y.tab.h permsyms.def.c am1 am1test fastload symtabgen cvtxldr \
	xldr.rim xldr.lst parser.output .y.tab.c.old .y.tab.h.old .lex.yy.c.old .permsyms.def.c.old

install: am1 fastload
	cp am1 ../../bin
	cp fastload ../../bin
	strip ../../bin/am1
	strip ../../bin/fastload

permsyms.def.o: permsyms.def.c
	$(CC) -c permsyms.def.c

lex.yy.o: lex.yy.c y.tab.h lexfuncs.c
	$(CC) -c lex.yy.c

y.tab.o: y.tab.c
	$(CC) -DYYDEBUG=1 -c y.tab.c

permsyms.def.c: symtabgen permsyms.def
	./symtabgen permsyms.def

lex.yy.c: lexer.l y.tab.h
	flex -d lexer.l

y.tab.%: parser.y am1.h symtab.h
	bison $(BISONFLAGS) -o y.tab.c -d parser.y

am1.o: am1.c am1.h symtab.h y.tab.h optimizer.h
	$(CC) -c am1.c

optimizer.o: optimizer.c optimizer.h am1.h y.tab.h
	$(CC) -c optimizer.c

am1-test.o: am1.c am1.h symtab.h y.tab.h optimizer.h
	$(CC) -DAM1_TEST_SWITCHES -c am1.c -o am1-test.o

optimizer-test.o: optimizer.c optimizer.h am1.h y.tab.h
	$(CC) -DAM1_TEST_SWITCHES -c optimizer.c -o optimizer-test.o

optdecode.o: optdecode.c optimizer.h am1.h y.tab.h
	$(CC) -c optdecode.c

optrefs.o: optrefs.c optimizer.h am1.h y.tab.h
	$(CC) -c optrefs.c

optflow.o: optflow.c optimizer.h am1.h y.tab.h
	$(CC) -c optflow.c

optreturn.o: optreturn.c optimizer.h am1.h y.tab.h
	$(CC) -c optreturn.c

optroutine.o: optroutine.c optimizer.h am1.h y.tab.h
	$(CC) -c optroutine.c

optcallgraph.o: optcallgraph.c optimizer.h am1.h y.tab.h
	$(CC) -c optcallgraph.c

optstrings.o: optstrings.c optimizer.h am1.h y.tab.h
	$(CC) -c optstrings.c

optrules.o: optrules.c optimizer.h am1.h y.tab.h
	$(CC) -c optrules.c

optshare.o: optshare.c optimizer.h am1.h y.tab.h
	$(CC) -c optshare.c

optregion.o: optregion.c optimizer.h am1.h y.tab.h
	$(CC) -c optregion.c

optreport.o: optreport.c optimizer.h am1.h y.tab.h
	$(CC) -c optreport.c

opttransform.o: opttransform.c optimizer.h am1.h symtab.h y.tab.h
	$(CC) -c opttransform.c

optvalue.o: optvalue.c optimizer.h am1.h y.tab.h
	$(CC) -c optvalue.c

optscratch.o: optscratch.c optimizer.h am1.h symtab.h y.tab.h
	$(CC) -c optscratch.c

optguess.o: optguess.c optimizer.h am1.h y.tab.h
	$(CC) -c optguess.c

optspeed.o: optspeed.c optimizer.h am1.h y.tab.h
	$(CC) -c optspeed.c

optwindow.o: optwindow.c optimizer.h am1.h symtab.h y.tab.h
	$(CC) -c optwindow.c

optrelayout.o: optrelayout.c optimizer.h am1.h symtab.h y.tab.h
	$(CC) -c optrelayout.c

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

srccodegen.o: srccodegen.c optimizer.h am1.h y.tab.h type340chars.h
	$(CC) -c srccodegen.c

testcodegen.o: testcodegen.c am1.h y.tab.h
	$(CC) -c testcodegen.c

listSymtab.o: listSymtab.c am1.h symtab.h y.tab.h
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
