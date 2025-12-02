files := $(wildcard *.c)

%.o:%.c
	gcc $(INC) -c $< -o $@

all: $(files:.c=.o)

clean:
	-rm *.o
