CFLAGS=-Wall -Ofast

mandelbrot: mandelbrot.c
	$(CC) $(CFLAGS) -o mandelbrot mandelbrot.c -lSDL2 -pthread

all: mandelbrot
