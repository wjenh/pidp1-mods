CC=gcc -O0 -g

all: disassembler

install: dosassembler
	cp disassembler ../../bin/disassemble_tape
	strip ../../bin/disassemble_tape

disassembler: disassemble_tape.o decode_instruction.o
	$(CC) -o disassemble_tape disassemble_tape.o decode_instruction.o

disassemble_tape.o: disassemble_tape.c
	$(CC) -c disassemble_tape.c

decode_instruction.o: decode_instruction.c
	$(CC) -c decode_instruction.c

clean:
	@rm disassembler *.o 2>/dev/null || true
