all: p7sim p7simES

p7sim: main.o glad/glad.o checkConfig.c ../blincolnlights/logger.c
	cc -O3 -Wno-unused-result -o $@ $^ -lm -ldl -lpthread `sdl2-config --cflags --libs`
p7simES: main.o glad/glad.o checkConfig.c ../blincolnlights/logger.c
	cc -O3 -Wno-unused-result -o $@ -DGLES $^ -lm -ldl -lpthread `sdl2-config --cflags --libs`

main.o: main.c
	cc -O3 -Wno-unused-result -c main.c `sdl2-config --cflags --libs`

clean:
	@rm *.o p7sim p7simES glad/glad.o 2>/dev/null || true
