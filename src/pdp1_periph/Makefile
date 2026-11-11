#CC=gcc
CC=gcc -w

all: pdp1_periph pdp1_periphES

SRC=main.c p7.c ptape.c typewriter.c ../common/common.c checkConfig.c \
	../common/logger.c
INC=-I../common
LIBS=-lSDL2_ttf

pdp1_periph: $(SRC) glad/glad.o
	$(CC) -O3 $(INC) -o $@ -g $^ -lm -ldl -lpthread `sdl2-config --cflags --libs` $(LIBS)
pdp1_periphES: $(SRC) glad/glad.o
	$(CC) -O3 $(INC) -o $@ -g -DGLES $^ -lm -ldl -lpthread `sdl2-config --cflags --libs` $(LIBS)
