all: disassembler decode_instruction.o

disassembler: disassemble_tape.c
	cc -o disassembler disassemble_tape.c

decode_instruction.o: decode_instruction.c

clean:
	-rm disassembler decode_instruction.o
