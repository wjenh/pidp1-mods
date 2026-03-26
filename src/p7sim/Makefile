all: p7sim p7simES

p7sim: main.o glad/glad.o ../blincolnlights/checkConfig.c ../blincolnlights/logger.c
	cc -g -O3 -Wno-unused-result -o $@ -g $^ -lm -ldl -lpthread `sdl2-config --cflags --libs`
p7simES: main.o glad/glad.o ../blincolnlights/checkConfig.c ../blincolnlights/logger.c
	cc -g -O3 -Wno-unused-result -o $@ -g -DGLES $^ -lm -ldl -lpthread `sdl2-config --cflags --libs`

main.o: main.c
	cc -g -O3 -Wno-unused-result -c main.c `sdl2-config --cflags --libs`

clean:
	@rm *.o p7sim p7simES glad/glad.o 2>/dev/null || true
