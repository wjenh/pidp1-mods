CC=gcc -O0 -g

all: disassemble_tape disassemble_mem

install: disassemble_tape disassemble_mem
	cp disassemble_tape ../../bin/disassemble_tape
	strip ../../bin/disassemble_tape
	cp disassemble_mem ../../bin/disassemble_mem
	strip ../../bin/disassemble_mem

disassemble_tape: disassemble_tape.o decode_instruction.o
	$(CC) -o disassemble_tape disassemble_tape.o decode_instruction.o

disassemble_mem: disassemble_mem.o decode_instruction.o
	$(CC) -o disassemble_mem disassemble_mem.o decode_instruction.o

disassemble_tape.o: disassemble_tape.c
	$(CC) -c disassemble_tape.c

disassemble_mem.o: disassemble_mem.c
	$(CC) -c disassemble_mem.c

decode_instruction.o: decode_instruction.c
	$(CC) -c decode_instruction.c

clean:
	@rm disassemble_tape disassemble_mem *.o 2>/dev/null || true
