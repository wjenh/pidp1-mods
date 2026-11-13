CC=gcc -O3
#CC=gcc -g

all: mtp fstool

install: mtp fstool
	cp mtp ../../bin/mtp
	strip ../../bin/mtp
	cp fstool ../../bin/fstool
	strip ../../bin/fstool

mtp: mtp.c
	$(CC) mtp.c -o mtp

fstool: fstool.c
	$(CC) fstool.c -o fstool

clean:
	@rm mtp fstool *.o 2>/dev/null || true
