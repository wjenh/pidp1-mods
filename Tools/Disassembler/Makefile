CC=gcc -O0 -g

all: disassemble

install: disassemble
	cp disassemble ../../bin/disassemble
	strip ../../bin/disassemble

disassemble: disassemble.o decode_instruction.o kloader.o am1loader.o binloader.o memloader.o
	$(CC) -o disassemble disassemble.o decode_instruction.o kloader.o am1loader.o binloader.o memloader.o

disassemble.o: disassemble.c loader.h loaderdefs.h decode_instruction.h
	$(CC) -c disassemble.c

decode_instruction.o: decode_instruction.c decode_instruction.h
	$(CC) -c decode_instruction.c

kloader.o: kloader.c loader.h
	$(CC) -c kloader.c

am1loader.o: am1loader.c loader.h
	$(CC) -c am1loader.c

binloader.o: binloader.c loader.h
	$(CC) -c binloader.c

memloader.o: memloader.c loader.h
	$(CC) -c memloader.c

clean:
	@rm disassemble *.o 2>/dev/null || true
