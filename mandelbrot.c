#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <getopt.h>
#include <SDL2/SDL.h>

#ifdef NO_LIBPNG
#define LIBPNG_GETOPT ""
#else
#define LIBPNG_GETOPT "r:"
#include <png.h>
#endif

#define DEFAULT_ITER_COUNT 768

#define OPT_CLEAR 1
#define THREAD_BUSY 1
#define THREAD_BEGIN 2
#define THREAD_EXIT 4

#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))

#define SOFTWARE_NAME_FOR_METADATA "MandelbrotSDL by flarn2006"
#define FRACTAL_INFO_TEXT_KEY "FractalInfo"
#define MSG_SAVED_SCREENSHOT "Saved screenshot to %s\n"
#define SCREENSHOT_NAME_FMT_WO_EXT "mandel%u"

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

int get_next_filename(char *filename, size_t filename_size, const char *format)
{
	int screenshot_file_num = 0;
	int found = 0;
	int snprintf_result;
	while (!found) {
		snprintf_result = snprintf(filename, filename_size, format, screenshot_file_num);
		FILE *fp = fopen(filename, "r");
		if (fp) {
			fclose(fp);
		} else if (errno == ENOENT) {
			found = 1;
		}
		++screenshot_file_num;
	}
	return snprintf_result;
}

#ifndef NO_LIBPNG
int init_from_png(const char *filename, struct options *opts, struct view_range *view)
{
	FILE *fp = fopen(filename, "rb");
	if (!fp) {
		perror(filename);
		return 1;
	}

	int retval = 0;
	png_structrp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_inforp info = png_create_info_struct(png);
	if (setjmp(png_jmpbuf(png))) {
		fprintf(stderr, "Error reading PNG\n");
		retval = 1;
		goto init_from_png_exit;
	}

	unsigned char sig[8];
	if (fread(sig, 1, 8, fp) != 8 || !png_check_sig(sig, 8)) {
		fprintf(stderr, "PNG signature not found; %s does not appear to be a PNG file.\n", filename);
		retval = 1;
	} else {
		png_set_sig_bytes(png, 8);
	}

	png_init_io(png, fp);
	png_read_info(png, info);
	png_textp text;
	int count = png_get_text(png, info, &text, NULL);
    int iterations;
	int found = 0;
	for (int i=0; i<count; ++i) {
		if (!strcmp(text[i].key, FRACTAL_INFO_TEXT_KEY)) {
			found = 1;
			if (sscanf(text[i].text, "%Lg,%Lg,%Lg,%Lg,%d", &view->xmin, &view->xmax, &view->ymin, &view->ymax, &iterations) < 5) {
				fprintf(stderr, "Invalid " FRACTAL_INFO_TEXT_KEY " format in %s.\n", filename);
				retval = 1;
				goto init_from_png_exit;
			} else if (opts->iterations == -1) {
				opts->iterations = iterations;
			}
			break;
		}
	}
	if (!found) {
		fprintf(stderr, "%s does not contain fractal information.\n", filename);
		retval = 1;
		goto init_from_png_exit;
	}

init_from_png_exit:
	png_destroy_read_struct(&png, &info, NULL);
	fclose(fp);
	return retval;
}
#endif

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
	opts.iterations = -1;  /* in case there's a "-r" option given */

	int opt; while ((opt = getopt(argc, argv, "w:h:i:p:t:c" LIBPNG_GETOPT)) != -1) {
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
#ifndef NO_LIBPNG
		case 'r': {
			int status = init_from_png(optarg, &opts, &view);
			if (status)
				return status;
			break;
		}
#endif
		case 't':
			opts.threads = atoi(optarg);
			break;
		case 'c':
			opts.flags |= OPT_CLEAR;
		}
	}
	if (opts.iterations == -1)
		opts.iterations = DEFAULT_ITER_COUNT;
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
#ifndef NO_LIBPNG
					int save_successful = 0;
					get_next_filename(filename, sizeof(filename), SCREENSHOT_NAME_FMT_WO_EXT ".png");
					FILE *fp = fopen(filename, "w");
					if (fp) {
						SDL_Surface *pngsfc = NULL;
						png_bytepp pngrows = NULL;

						png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
                        png_infop info = png_create_info_struct(png);
						if (setjmp(png_jmpbuf(png))) {
							fclose(fp);
							fprintf(stderr, "Error creating PNG; falling back to BMP format.\n");
							remove(filename);
							get_next_filename(filename, sizeof(filename), SCREENSHOT_NAME_FMT_WO_EXT ".bmp");
							fp = fopen(filename, "wb");
							if (fp) {
								if (SDL_SaveBMP_RW(sfc, SDL_RWFromFP(fp, SDL_FALSE), 0) == 0)
									save_successful = 1;
								else
									perror(filename);
							} else {
								perror(filename);
							}
						} else {
							png_init_io(png, fp);
							png_set_IHDR(png, info, opts.width, opts.height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

#ifdef TEST_PNG_ERROR
							png_error(png, "Fake error for testing");
#endif
							
							struct png_text_struct text[2];
							text[0].compression = PNG_TEXT_COMPRESSION_NONE;
							text[0].key = "Software";
							text[0].lang = NULL;
							text[0].lang_key = NULL;
							text[0].text = SOFTWARE_NAME_FOR_METADATA;
							text[0].text_length = strlen(text[0].text);

							text[1].compression = PNG_TEXT_COMPRESSION_zTXt;
							text[1].key = FRACTAL_INFO_TEXT_KEY;
							text[1].lang = NULL;
							text[1].lang_key = NULL;
							char textbuf[128];
							text[1].text_length = snprintf(textbuf, sizeof(textbuf), "%.20Lg,%.20Lg,%.20Lg,%.20Lg,%d", view.xmin, view.xmax, view.ymin, view.ymax, opts.iterations);
							text[1].text = textbuf;
							png_set_text(png, info, text, 2);

							pngsfc = SDL_CreateRGBSurface(0, opts.width, opts.height, 24, 0xFF, 0xFF00, 0xFF0000, 0);
							SDL_BlitSurface(sfc, NULL, pngsfc, NULL);
							pngrows = calloc(opts.height, sizeof(png_const_bytep));
							for (i=0; i<opts.height; ++i) {
								pngrows[i] = pngsfc->pixels + 3 * opts.width * i;
							}
							png_set_rows(png, info, pngrows);

							png_write_png(png, info, 0, NULL);
							save_successful = 1;
						}

						if (save_successful) printf(MSG_SAVED_SCREENSHOT, filename);
						if (fp) fclose(fp);
						png_destroy_write_struct(&png, &info);
						if (pngrows) free(pngrows);
						if (pngsfc) SDL_FreeSurface(pngsfc);
					} else {
						perror(filename);
					}
#else
					get_next_filename(filename, sizeof(filename), SCREENSHOT_NAME_FMT_WO_EXT ".bmp");
					if (SDL_SaveBMP(sfc, filename) == 0)
						printf(MSG_SAVED_SCREENSHOT, filename);
					else
						perror(filename);
#endif
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
