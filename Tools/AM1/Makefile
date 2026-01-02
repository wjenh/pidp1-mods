CC=gcc

am1:	am1.o permsyms.def.o symtab.o lex.yy.o y.tab.o parsefns.o permsyms.def.o eval.o \
	maccodegen.o bincodegen.o listcodegen.o

clean:
	@rm *.o lex.yy.c y.tab.c y.tab.h permsyms.def.c am1 symtabgen 2>/dev/null || true

permsyms.def.o: permsyms.def.c
	$(CC) -c permsyms.def.c

permsyms.def.c: symtabgen permsyms.def
	./symtabgen permsyms.def

lex.yy.o: lex.yy.c y.tab.h
	$(CC) -c lex.yy.c
	
lex.yy.c: lexer.l y.tab.c lexfuncs.c
	flex -d lexer.l

y.tab.c, y.tab.h: parser.y am1.h symtab.h
	bison -o y.tab.c -d parser.y

y.tab.o: y.tab.c
	$(CC) -DYYDEBUG=1 -c y.tab.c

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

symtabgen: symtabgen.c am1.h symtab.o
	$(CC) -o symtabgen symtabgen.c symtab.o

symtab.o: symtab.c symtab.h
	$(CC) -c symtab.c
