CFLAGS= $(INC) -O3 -Wno-unused-result
SRC := $(wildcard *.c)
OBJ := $(SRC:.c=.o)

all: $(OBJ)
	cc -c $(CFLAGS) $(SRC)

clean:
	@rm *.o >/dev/null 2>&1 || true
