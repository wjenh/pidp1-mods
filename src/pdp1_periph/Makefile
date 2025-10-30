all: pdp1_periph pdp1_periphES

SRC=main.c p7.c ptape.c typewriter.c ../blincolnlights/common.c
INC=-I../blincolnlights -I../src/blincolnlights/pdp1

pdp1_periph: $(SRC) glad/glad.o
	cc -g -O3 $(INC) -o $@ -g $^ -lm -ldl -lpthread `sdl2-config --cflags --libs` $(LIBS)
pdp1_periphES: $(SRC) glad/glad.o
	cc -g -O3 $(INC) -o $@ -g -DGLES $^ -lm -ldl -lpthread `sdl2-config --cflags --libs` $(LIBS)
