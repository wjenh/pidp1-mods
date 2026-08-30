CC=gcc -O0 -g -Wall

all: wz

install: wz
	strip wz
	cp wz ..

wz: wz.c
	$(CC) wz.c -o wz

clean:
	@rm -f wz 2>/dev/null || true
