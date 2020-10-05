#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <getopt.h>
#include <SDL2/SDL.h>
#include <png.h>

#define DEFAULT_ITER_COUNT 768

#define OPT_CLEAR 1
#define THREAD_BUSY 1
#define THREAD_BEGIN 2
#define THREAD_EXIT 4

#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))

typedef long double coord_t;

struct options {
	short flags;
	int width;
	int height;
	int iterations;
	Uint32 *colormap;
	size_t palsize;
	int threads;
};

struct view_range {
	coord_t xmin;
	coord_t xmax;
	coord_t ymin;
	coord_t ymax;
};

struct thread_data {
	short flags;
	pthread_t thread;
	int index;
	SDL_Surface *sfc;
	const struct options *opts;
	struct view_range view;
	pthread_cond_t *cond;
	pthread_mutex_t mutex;
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

#ifdef DEBUG_THREAD_STATUS
#define PRINT_THREAD_STATUS(status) printf("Thread %d " status "\n", td->index)
#else
#define PRINT_THREAD_STATUS(status)
#endif

void *thread_main(void *arg)
{
	struct thread_data *td = arg;

	for (;;) {
		PRINT_THREAD_STATUS("waiting...");
		while (!(td->flags & (THREAD_BEGIN | THREAD_EXIT))) {
			pthread_cond_wait(td->cond, &td->mutex);
		}
		if (td->flags & THREAD_EXIT)
			break;

		td->flags &= ~THREAD_BEGIN;
		td->flags |= THREAD_BUSY;

		PRINT_THREAD_STATUS("working...");
		int y; for (y=td->index; y<td->opts->height; y+=td->opts->threads) {
			if (td->flags & (THREAD_BEGIN | THREAD_EXIT)) {
				PRINT_THREAD_STATUS("interrupted!");
				break;
			} else {
				coord_t yy = map(y, td->opts->height-1, 0, td->view.ymin, td->view.ymax);
				generate_row(y, td->sfc, td->opts, td->view.xmin, td->view.xmax, yy);
			}
		}
		td->flags &= ~THREAD_BUSY;
		PRINT_THREAD_STATUS("finished.");
	}

	PRINT_THREAD_STATUS("exiting.");
	return NULL;
}

void generate_fractal(SDL_Surface *sfc, const struct options *opts, struct view_range *view, struct thread_data *threads, pthread_cond_t *cond)
{
	int i; for (i=0; i<opts->threads; ++i) {
		threads[i].view.xmin = view->xmin;
		threads[i].view.xmax = view->xmax;
		threads[i].view.ymin = view->ymin;
		threads[i].view.ymax = view->ymax;
		threads[i].flags |= THREAD_BEGIN;
	}
	pthread_cond_broadcast(cond);
}

void init_options(struct options *opts, struct view_range *view)
{
	opts->iterations = DEFAULT_ITER_COUNT;
	view->xmin = -2.0;
	view->xmax = 1.0;
	view->ymin = 1.0;
	view->ymax = -1.0;
}

int main(int argc, char *argv[])
{
	struct options opts;
	opts.flags = 0;
	opts.width = 1200;
	opts.height = 800;
	opts.threads = 4;

	pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

	FILE *palette_file = NULL;

	struct view_range view;
	init_options(&opts, &view);

	int opt; while ((opt = getopt(argc, argv, "w:h:i:p:t:c")) != -1) {
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
			break;
		case 'c':
			opts.flags |= OPT_CLEAR;
		}
	}
	if (opts.threads < 1 || opts.threads > opts.height) {
		fprintf(stderr, "%s: thread count must be between 1 and the current height (%d)\n", argv[0], opts.height);
		return 2;
	}

	struct thread_data *threads = calloc(opts.threads, sizeof(struct thread_data));
	int i; for (i=0; i<opts.threads; ++i) {
		threads[i].flags = 0;
		threads[i].index = i;
		threads[i].opts = &opts;
		threads[i].view.xmin = view.xmin;
		threads[i].view.xmax = view.xmax;
		threads[i].view.ymin = view.ymin;
		threads[i].view.ymax = view.ymax;
		threads[i].cond = &cond;
		threads[i].mutex = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
		pthread_create(&threads[i].thread, NULL, thread_main, &threads[i]);
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
	for (i=0; i<opts.threads; ++i) {
		threads[i].sfc = sfc;
	}

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

	generate_fractal(sfc, &opts, &view, threads, &cond);
	SDL_UpdateWindowSurface(win);

	SDL_Event event;
	int screenshot_file_num = 0;
	int quit = 0; while (!quit) {
		while (SDL_PollEvent(&event) != 0) {
			int update_view = 0;
			sfc = SDL_GetWindowSurface(win);

			if (event.type == SDL_QUIT) {
				putchar('\n');
				quit = 1;
			} else if (event.type == SDL_MOUSEBUTTONDOWN) {
				if (event.button.button == SDL_BUTTON_LEFT) {
					coord_t xx = map(event.button.x, 0, opts.width-1, view.xmin, view.xmax);
					coord_t yy = map(event.button.y, opts.height-1, 0, view.ymin, view.ymax);
					view.xmin = (view.xmin + xx) / 2;
					view.xmax = (xx + view.xmax) / 2;
					view.ymin = (view.ymin + yy) / 2;
					view.ymax = (yy + view.ymax) / 2;
					update_view = 1;
				} else if (event.button.button == SDL_BUTTON_MIDDLE) {
					init_options(&opts, &view);
					update_view = 1;
				} else if (event.button.button == SDL_BUTTON_RIGHT) {
					coord_t xx = map(event.button.x, 0, opts.width-1, view.xmin, view.xmax);
					coord_t yy = map(event.button.y, opts.height-1, 0, view.ymin, view.ymax);
					view.xmin = view.xmin * 2 - xx;
					view.xmax = view.xmax * 2 - xx;
					view.ymin = view.ymin * 2 - yy;
					view.ymax = view.ymax * 2 - yy;
					update_view = 1;
				}
			} else if (event.type == SDL_KEYDOWN) {
				if (event.key.keysym.sym == SDLK_s) {
					char filename[32];
					int found = 0;
					while (!found) {
						snprintf(filename, 16, "mandel%u.png", screenshot_file_num);
						FILE *fp = fopen(filename, "r");
						if (fp) {
							fclose(fp);
						} else if (errno == ENOENT) {
							found = 1;
						}
						++screenshot_file_num;
					}

					FILE *fp = fopen(filename, "w");
					if (fp) {
						SDL_Surface *pngsfc = NULL;
						png_bytepp pngrows = NULL;

						png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
                        png_infop info = png_create_info_struct(png);
						if (setjmp(png_jmpbuf(png))) {
							fprintf(stderr, "Error creating PNG\n");
						} else {
							png_init_io(png, fp);
							png_set_IHDR(png, info, opts.width, opts.height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
							
							struct png_text_struct text;
							text.compression = PNG_TEXT_COMPRESSION_NONE;
							text.key = "FractalInfo";
							text.lang = NULL;
							text.lang_key = NULL;
							char textbuf[128];
							text.text_length = snprintf(textbuf, sizeof(textbuf), "%.20Lg,%.20Lg,%.20Lg,%.20Lg,%d", view.xmin, view.xmax, view.ymin, view.ymax, opts.iterations);
							text.text = textbuf;
							png_set_text(png, info, &text, 1);

							pngsfc = SDL_CreateRGBSurface(0, opts.width, opts.height, 24, 0xFF, 0xFF00, 0xFF0000, 0);
							SDL_BlitSurface(sfc, NULL, pngsfc, NULL);
							pngrows = calloc(opts.height, sizeof(png_const_bytep));
							for (i=0; i<opts.height; ++i) {
								pngrows[i] = pngsfc->pixels + 3 * opts.width * i;
							}
							png_set_rows(png, info, pngrows);

							png_write_png(png, info, 0, NULL);
							printf("Saved screenshot to %s\n", filename);
						}

						fclose(fp);
						png_destroy_write_struct(&png, &info);
						if (pngrows) free(pngrows);
						if (pngsfc) SDL_FreeSurface(pngsfc);
					} else {
						perror(filename);
					}
				} else if (event.key.keysym.sym == SDLK_c) {
					printf("Xmin = % 2.20Lf\n", view.xmin);
					printf("Xmax = % 2.20Lf\n", view.xmax);
					printf("Ymin = % 2.20Lf\n", view.ymin);
					printf("Ymax = % 2.20Lf\n\n", view.ymax);
					printf("Iter = %d\n", opts.iterations);
				} else if (event.key.keysym.sym == SDLK_i) {
					opts.iterations += DEFAULT_ITER_COUNT;
					update_view = 1;
				}
			}

			if (update_view) {
				if (opts.flags & OPT_CLEAR) {
					Uint32 color = SDL_MapRGB(sfc->format, 32, 32, 32);
					SDL_Rect rect;
					rect.x = rect.y = 0;
					rect.w = opts.width;
					rect.h = opts.height;
					SDL_FillRect(sfc, &rect, color);
				}
				generate_fractal(sfc, &opts, &view, threads, &cond);
			}
		}
        SDL_UpdateWindowSurface(win);
	}

	for (i=0; i<opts.threads; ++i) 
		threads[i].flags |= THREAD_EXIT;
	
	pthread_cond_broadcast(&cond);

	for (i=0; i<opts.threads; ++i) {
		pthread_join(threads[i].thread, NULL);
		pthread_mutex_destroy(&threads[i].mutex);
	}
	
	pthread_cond_destroy(&cond);

	free(threads);
	free(opts.colormap);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
