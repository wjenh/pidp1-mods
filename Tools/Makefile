INC= -I../src/blincolnlights -I../src/blincolnlights/pdp1

all: pdp1audio M1PP/m1pp

pdp1audio: pdp1audio.c
	gcc $(INC) pdp1audio.c ../src/blincolnlights/common.c -o pdp1audio

specialsyms.sym: specialsyms.mac
	../bin/macro1_1 -s specialsyms.mac
	rm specialsyms.rim specialsyms.lst

install: pdp1audio specialsyms.sym M1PP/m1pp
	cp pdp1audio ../bin
	cp M1PP/m1pp ../bin

M1PP/m1pp:
	(cd M1PP; make)

clean:
	rm pdp1audio M1PP/m1pp
