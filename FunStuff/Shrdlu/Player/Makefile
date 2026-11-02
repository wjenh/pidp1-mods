# shrdlu_play is a scripted telnet client for SHRDLU. "make" builds it, needing gcc and nothing else.

CC = gcc
CFLAGS = -std=gnu17 -O2 -Wall -Wextra

shrdlu_play: shrdlu_play.c
	$(CC) $(CFLAGS) shrdlu_play.c -o $@

install: shrdlu_play
	cp shrdlu_play ..

clean:
	rm -f shrdlu_play

.PHONY: clean
