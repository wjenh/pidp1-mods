CC=gcc -O0 -g
BISONFLAGS=
#CONFLICTS=-Wcounterexamples

all: ad1

ad1: ad1.o helpText.o eval.o lex.yy.o y.tab.o listFile.o files.o commands.o flexToAscii.o \
	../Disassembler/decode_instruction.o loader.o
	$(CC) -o ad1 ad1.o helpText.o eval.o lex.yy.o y.tab.o listFile.o files.o commands.o flexToAscii.o \
	../Disassembler/decode_instruction.o loader.o

clean:
	@rm ad1 *.o y.tab.c y.tab.h lex.yy.c 2>/dev/null || true

install: ad1
	cp ad1 ../../bin
	strip ../../bin/ad1

ad1.o: ad1.c helpmsgs.h y.tab.h ad1.h
	$(CC) -c ad1.c

helpText.o: helpText.c helpmsgs.h ad1.h
	$(CC) -c helpText.c

files.o: files.c ad1.h
	$(CC) -c files.c

eval.o: eval.c ad1.h y.tab.h ad1.h
	$(CC) -c eval.c

commands.o: commands.c helpmsgs.h y.tab.h ad1.h
	$(CC) -c commands.c

listFile.o: listFile.c ad1.h
	$(CC) -c listFile.c

loader.o: loader.c ad1.h
	$(CC) -c loader.c

flexToAscii.o: flexToAscii.c
	$(CC) -c flexToAscii.c

../Disassembler/decode_instruction.o: ../Disassembler/decode_instruction.c
	(cd ../Disassembler ; make decode_instruction.o)

lex.yy.o: lex.yy.c y.tab.h
	$(CC) -c lex.yy.c
	
lex.yy.c: lexer.l y.tab.c
	flex -d lexer.l

y.tab.h: y.tab.o

y.tab.c: parser.y pdp1inc.h ad1.h
	bison $(BISONFLAGS) $(CONFLICTS) -o y.tab.c -d parser.y

y.tab.o: y.tab.c
	$(CC) -DYYDEBUG=1 -c y.tab.c
