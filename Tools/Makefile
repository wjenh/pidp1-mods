INC= -I../src/blincolnlights -I../src/blincolnlights/pdp1

pdp1audio: pdp1audio.c
	gcc $(INC) pdp1audio.c ../src/blincolnlights/common.c -o pdp1audio

install: pdp1audio
	cp pdp1audio ../bin

clean:
	rm *.o pdp1audio
