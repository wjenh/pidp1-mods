all: pdp1_periph pdp1_periphES

SRC=main.c p7.c ptape.c ../blincolnlights/common.c
INC=-I../blincolnlights

pdp1_periph: $(SRC) glad/glad.o
	cc -g -O3 $(INC) -o $@ -g $^ -lm -ldl -lpthread `sdl2-config --cflags --libs`
pdp1_periphES: $(SRC) glad/glad.o
	cc -g -O3 $(INC) -o $@ -g -DGLES $^ -lm -ldl -lpthread `sdl2-config --cflags --libs`
