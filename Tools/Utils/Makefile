CC=gcc -O3
#CC=gcc -g

all: mtp fstool asr33tape

install: mtp fstool
	cp mtp ../../bin/mtp
	strip ../../bin/mtp
	cp fstool ../../bin/fstool
	strip ../../bin/fstool
	cp asr33tape ../../bin/asr33tape
	strip ../../bin/asr33tape

mtp: mtp.c
	$(CC) mtp.c -o mtp

fstool: fstool.c
	$(CC) fstool.c -o fstool

asr33tape: asr33tape.c
	$(CC) asr33tape.c -o asr33tape

clean:
	@rm mtp fstool asr33tape *.o 2>/dev/null || true
