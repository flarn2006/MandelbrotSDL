#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <getopt.h>
#include <SDL2/SDL.h>

#define MAX_THREADS 1024  /* It won't actually create this many threads unless the user tells it to. */
#define DEFAULT_ITER_COUNT 768

#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))

typedef long double coord_t;

struct options {
	int width;
	int height;
	int iterations;
	Uint32 *colormap;
	size_t palsize;
	int threads;
};

struct thread_data {
	int index;
	SDL_Surface *sfc;
	const struct options *opts;
	coord_t xmin;
	coord_t xmax;
	coord_t ymin;
	coord_t ymax;
};

coord_t map(coord_t value, coord_t inputMin, coord_t inputMax, coord_t outputMin, coord_t outputMax)
{
	coord_t x = (value - inputMin) / (inputMax - inputMin);
	return x * (outputMax - outputMin) + outputMin;
}

void generate_row(int rownum, SDL_Surface *sfc, const struct options *opts, coord_t xmin, coord_t xmax, coord_t y)
{
	int x; for (x=0; x<opts->width; ++x) {
		coord_t xx = map(x, 0, opts->width-1, xmin, xmax);
		
		coord_t zr = 0.0;
		coord_t zi = 0.0;
		
		int i = 0;
		while (i < opts->iterations && zr*zr + zi*zi < 4) {
			++i;
			coord_t zr_new = zr*zr - zi*zi + xx;
			zi = 2*zr*zi + y;
			zr = zr_new;
		}

		SDL_Rect rect;
		rect.x = x; rect.y = rownum;
		rect.w = 1; rect.h = 1;
		if (i < opts->iterations)
			SDL_FillRect(sfc, &rect, opts->colormap[i % opts->palsize]);
		else
			SDL_FillRect(sfc, &rect, SDL_MapRGB(sfc->format, 0, 0, 0));
	}
}

void *thread_start(void *arg)
{
	struct thread_data *td = arg;
	int y; for (y=td->index; y<td->opts->height; y+=td->opts->threads) {
		coord_t yy = map(y, td->opts->height-1, 0, td->ymin, td->ymax);
		generate_row(y, td->sfc, td->opts, td->xmin, td->xmax, yy);
	}
	return NULL;
}

void generate_fractal(SDL_Surface *sfc, const struct options *opts, coord_t xmin, coord_t xmax, coord_t ymin, coord_t ymax)
{
	pthread_t threads[MAX_THREADS];
	struct thread_data td[MAX_THREADS];

	int i; for (i=0; i<opts->threads; ++i) {
		td[i].index = i;
		td[i].sfc = sfc;
		td[i].opts = opts;
		td[i].xmin = xmin;
		td[i].xmax = xmax;
		td[i].ymin = ymin;
		td[i].ymax = ymax;
		pthread_create(&threads[i], NULL, thread_start, &td[i]);
	}
	
	for (i=0; i<opts->threads; ++i) {
		pthread_join(threads[i], NULL);
	}
}

void init_options(struct options *opts, coord_t *xmin, coord_t *xmax, coord_t *ymin, coord_t *ymax)
{
	opts->iterations = DEFAULT_ITER_COUNT;
	*xmin = -2.0;
	*xmax = 1.0;
	*ymin = 1.0;
	*ymax = -1.0;
}

int main(int argc, char *argv[])
{
	struct options opts;
	opts.width = 1200;
	opts.height = 800;
	opts.threads = 4;

	FILE *palette_file = NULL;

	int opt; while ((opt = getopt(argc, argv, "w:h:i:p:t:")) != -1) {
		switch (opt) {
		case 'w':
			opts.width = atoi(optarg);
			if (opts.width <= 0) {
				fprintf(stderr, "%s: width must be at least 1\n", argv[0]);
				return 2;
			}
			break;
		case 'h':
			opts.height = atoi(optarg);
			if (opts.height <= 0) {
				fprintf(stderr, "%s: height must be at least 1\n", argv[0]);
				return 2;
			}
			break;
		case 'i':
			opts.iterations = atoi(optarg);
			if (opts.iterations <= 0) {
				fprintf(stderr, "%s: iterations must be at least 1\n", argv[0]);
				return 2;
			}
			break;
		case 'p':
			palette_file = fopen(optarg, "rb");
			if (!palette_file) {
				perror(optarg);
				return 1;
			}
			break;
		case 't':
			opts.threads = atoi(optarg);
			if (opts.threads < 1 || opts.threads > MAX_THREADS) {
				fprintf(stderr, "%s: thread count must be between 1 and %d\n", argv[0], MAX_THREADS);
				return 2;
			}
		}
	}

	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		fprintf(stderr, "Error initializing SDL: %s\n", SDL_GetError());
		return 1;
	}

	SDL_Window *win = SDL_CreateWindow("Mandelbrot Set", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, opts.width, opts.height, SDL_WINDOW_SHOWN);
	if (!win) {
		fprintf(stderr, "Error creating window: %s\n", SDL_GetError());
		return 1;
	}

	SDL_Surface *sfc = SDL_GetWindowSurface(win);

	coord_t xmin, xmax, ymin, ymax;
	init_options(&opts, &xmin, &xmax, &ymin, &ymax);

	if (palette_file) {
		opts.colormap = calloc(opts.iterations, sizeof(Uint32));
		opts.palsize = 0;
		while (opts.palsize < opts.iterations) {
			Uint8 rgb[3];
			if (fread(rgb, 1, 3, palette_file) == 3)
				opts.colormap[opts.palsize] = SDL_MapRGB(sfc->format, rgb[0], rgb[1], rgb[2]);
			else
				break;
			++opts.palsize;
		}
		fclose(palette_file);
	} else {
		opts.palsize = DEFAULT_ITER_COUNT;
		opts.colormap = calloc(opts.palsize, sizeof(Uint32));

		int i; for (i=0; i<opts.palsize; ++i) {
			Uint8 r = max(512, i) - 512;
			Uint8 g = max(256, min(i, 511)) - 256;
			Uint8 b = min(i, 255);
			opts.colormap[i] = SDL_MapRGB(sfc->format, r, g, b);
		}
	}

	generate_fractal(sfc, &opts, xmin, xmax, ymin, ymax);
	SDL_UpdateWindowSurface(win);

	SDL_Event event;
	int screenshot_file_num = 0;
	int quit = 0; while (!quit) {
		while (SDL_PollEvent(&event) != 0) {
			int update_view = 0;

			if (event.type == SDL_QUIT) {
				putchar('\n');
				quit = 1;
			} else if (event.type == SDL_MOUSEBUTTONDOWN) {
				if (event.button.button == SDL_BUTTON_LEFT) {
					coord_t xx = map(event.button.x, 0, opts.width-1, xmin, xmax);
					coord_t yy = map(event.button.y, opts.height-1, 0, ymin, ymax);
					xmin = (xmin + xx) / 2;
					xmax = (xx + xmax) / 2;
					ymin = (ymin + yy) / 2;
					ymax = (yy + ymax) / 2;
					update_view = 1;
				} else if (event.button.button == SDL_BUTTON_RIGHT) {
					init_options(&opts, &xmin, &xmax, &ymin, &ymax);
					update_view = 1;
				}
			} else if (event.type == SDL_KEYDOWN) {
				if (event.key.keysym.sym == SDLK_s) {
					char filename[32];
					int found = 0;
					while (!found) {
						snprintf(filename, 16, "mandel%u.bmp", screenshot_file_num);
						FILE *fp = fopen(filename, "r");
						if (fp) {
							fclose(fp);
						} else if (errno == ENOENT) {
							found = 1;
						}
						++screenshot_file_num;
					}
					if (SDL_SaveBMP(sfc, filename) == 0) {
						printf("Saved screenshot to %s\n", filename);
					} else {
						fprintf(stderr, "Error saving %s: %s\n", filename, SDL_GetError());
					}
				} else if (event.key.keysym.sym == SDLK_c) {
					printf("Xmin = % 2.20Lf\n", xmin);
					printf("Xmax = % 2.20Lf\n", xmax);
					printf("Ymin = % 2.20Lf\n", ymin);
					printf("Ymax = % 2.20Lf\n\n", ymax);
					printf("Iter = %d\n", opts.iterations);
				} else if (event.key.keysym.sym == SDLK_i) {
					opts.iterations += DEFAULT_ITER_COUNT;
					update_view = 1;
				}
			}

			if (update_view) {
				sfc = SDL_GetWindowSurface(win);
				generate_fractal(sfc, &opts, xmin, xmax, ymin, ymax);
				SDL_UpdateWindowSurface(win);
			}
		}
	}

	free(opts.colormap);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
