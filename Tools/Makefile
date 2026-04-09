INC= -I../src/blincolnlights -I../src/blincolnlights/pdp1

pdp1audio: pdp1audio.c
	gcc $(INC) pdp1audio.c ../src/blincolnlights/common.c -o pdp1audio

tools:
	echo making mactoam1 and disassemble
	cd MtoA ; make ; cp mactoam1 ../../bin
	cd Disassembler ; make ; cp disassemble ../../bin

install: pdp1audio tools
	cp pdp1audio ../bin

clean:
	rm *.o pdp1audio
