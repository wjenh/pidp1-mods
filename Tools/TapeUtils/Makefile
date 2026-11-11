CC=gcc -O3
#CC=gcc -g

all: mtp mtfs

install: mtp mtfs
	cp mtp ../../bin/mtp
	strip ../../bin/mtp
	cp mtfs ../../bin/mtfs
	strip ../../bin/mtfs

mtp: mtp.c
	$(CC) mtp.c -o mtp

mtfs: mtfs.c
	$(CC) mtfs.c -o mtfs

clean:
	@rm mtp mtfs *.o 2>/dev/null || true
