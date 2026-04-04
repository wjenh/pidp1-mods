INC= -I../src/blincolnlights -I../src/blincolnlights/pdp1

pdp1audio: pdp1audio.c
	gcc $(INC) pdp1audio.c ../src/blincolnlights/common.c -o pdp1audio

tools:
	echo making am1, drumupdater, drumlist, and mtoa
	cd AM1 ; make ; cp am1 ../../bin
	cd Drumupdater ; make ; cp am1 ../../bin
	cd MtoA ; make ; cp am1 ../../bin
	cd Disassembler ; make ; cp disassembler ../../bin

install: pdp1audio
	cp pdp1audio ../bin

clean:
	rm *.o pdp1audio
