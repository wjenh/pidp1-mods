CC=gcc -O3
#CC=gcc -g

all: mtp

install: mtp
	cp mtp ../../bin/mtp
	strip ../../bin/mtp

mtp: mtp.c
	$(CC) mtp.c -o mtp

clean:
	@rm mtp *.o 2>/dev/null || true
