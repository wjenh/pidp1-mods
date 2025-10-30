all: p7sim p7simES

p7sim: main.c glad/glad.o
	cc -g -O3 -o $@ -g $^ -lm -ldl -lpthread `sdl2-config --cflags --libs`
p7simES: main.c glad/glad.o
	cc -g -O3 -o $@ -g -DGLES $^ -lm -ldl -lpthread `sdl2-config --cflags --libs`
