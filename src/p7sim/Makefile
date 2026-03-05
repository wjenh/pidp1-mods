all: p7sim p7simES

p7sim: main.c glad/glad.o ../blincolnlights/checkConfig.c ../blincolnlights/logger.c
	cc -g -O3 -Wno-unused-result -o $@ -g $^ -lm -ldl -lpthread `sdl2-config --cflags --libs`
p7simES: main.c glad/glad.o ../blincolnlights/checkConfig.c ../blincolnlights/logger.c
	cc -g -O3 -Wno-unused-result -o $@ -g -DGLES $^ -lm -ldl -lpthread `sdl2-config --cflags --libs`
