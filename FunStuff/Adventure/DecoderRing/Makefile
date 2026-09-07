#CC=gcc -O0 -g -Wall
CC=gcc -O3 -Wall

all: wz

install: wz
	cp wz ..
	strip ../wz

wz: wz.c
	$(CC) wz.c -o wz

clean:
	@rm -f wz 2>/dev/null || true
