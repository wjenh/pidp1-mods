CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11

all: zloader

install: all
	strip zloader
	cp zloader ../zloader

zloader: zloader.c zloader.h
	$(CC) $(CFLAGS) -o zloader zloader.c

clean:
	@rm -f zloader *.o
