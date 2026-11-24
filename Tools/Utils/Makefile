#CC=gcc -O3
CC=gcc -g

all: mtp fstool asr33tape dsk1301

install: mtp fstool asr33tape dsk1301
	cp mtp ../../bin/mtp
	strip ../../bin/mtp
	cp fstool ../../bin/fstool
	strip ../../bin/fstool
	cp asr33tape ../../bin/asr33tape
	strip ../../bin/asr33tape
	cp dsk1301 ../../bin/dsk1301
	strip ../../bin/dsk1301

mtp: mtp.c
	$(CC) mtp.c -o mtp

fstool: fstool.c
	$(CC) fstool.c -o fstool

asr33tape: asr33tape.c
	$(CC) asr33tape.c -o asr33tape

# dsk1301 shares the image layout with the IBM 1301 disk IOT.
dsk1301: dsk1301.c ../../IOTs/IBM1301Disk/ibm1301image.h
	$(CC) -Wall -I../../IOTs/IBM1301Disk dsk1301.c -o dsk1301

clean:
	@rm mtp fstool asr33tape dsk1301 *.o 2>/dev/null || true
