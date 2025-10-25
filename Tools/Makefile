INC= -I../src/blincolnlights -I../src/blincolnlights/pdp1

pdp1audio: pdp1audio.c
	gcc $(INC) pdp1audio.c ../src/blincolnlights/common.c -o pdp1audio

specialsyms.sym: specialsyms.mac
	../bin/macro1_1 -s specialsyms.mac
	rm specialsyms.rim specialsyms.lst

install: pdp1audio specialsyms.sym
	cp pdp1audio ../bin

clean:
	rm *.o pdp1audio
