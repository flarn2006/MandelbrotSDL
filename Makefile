CFLAGS=-Wall -Ofast $(shell sdl2-config --cflags) $(shell libpng-config --cflags)
LIBS=-pthread $(shell sdl2-config --libs) $(shell libpng-config --libs)

mandelbrot: mandelbrot.c
	$(CC) $(CFLAGS) -o mandelbrot mandelbrot.c $(LIBS)

clean:
	rm -f mandelbrot

all: mandelbrot
