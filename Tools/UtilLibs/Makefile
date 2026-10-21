# Shared code.
# Each tool compiles what it uses from here from its own Makefile,
# so the tools need nothing built here
# "make" here compiles every file just as a check.
# The link client needs the emulator's ad1proto.h, the wire format.
INC=-I../../src/blincolnlights/pdp1
files := $(wildcard *.c)

%.o:%.c
	gcc $(INC) -c $< -o $@

all: $(files:.c=.o)

clean:
	@rm -f *.o >/dev/null 2>&1
