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

#ifdef __linux__
#define USE_NPROCS
#endif
#ifdef USE_NPROCS
#include <sys/sysinfo.h>
#else
#define DEFAULT_THREADS 4
#endif

#define DEFAULT_WIDTH 1200
#define DEFAULT_HEIGHT 800
#define DEFAULT_ITER_COUNT 768

#define OPT_CLEAR 1
#define OPT_USER_Z0 2
#define THREAD_BUSY 1
#define THREAD_BEGIN 2
#define THREAD_EXIT 4

#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))

#define SOFTWARE_NAME_FOR_METADATA "MandelbrotSDL by flarn2006"
#define FRACTAL_INFO_TEXT_KEY "FractalInfo"
#define MSG_SAVED_SCREENSHOT "Saved screenshot to %s\n"
#define SCREENSHOT_NAME_FMT_WO_EXT "mandel%u"
#define DEFAULT_PALETTE_FILENAME_1 "generated.pal"
#define DEFAULT_PALETTE_FILENAME_2 "default.pal"

typedef long double coord_t;
#define CTFMT "L" /* for printf/scanf of coord_t: "%" CTFMT "f" (or "g") */

struct options {
	short flags;
	int width;
	int height;
	int *rowseq;
	int iterations;
	Uint32 *colormap;
	size_t palsize;
	int threads;
	coord_t z0r, z0i;
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

unsigned int reverse_bits(unsigned int n, int bits)
{
	unsigned int out = (bits % 2) ? (n & 1 << bits/2) : 0;
	for (int i=0; i<bits/2; ++i) {
		int shift = bits - (2*i + 1);
		out |= (n & 1 << i) << shift;
		out |= (n & 1 << (bits - (i + 1))) >> shift;
	}
	return out;
}

void generate_row(int rownum, SDL_Surface *sfc, const struct options *opts, coord_t xmin, coord_t xmax, coord_t y)
{
	int x; for (x=0; x<opts->width; ++x) {
		coord_t xx = map(x, 0, opts->width-1, xmin, xmax);
		
		coord_t zr = opts->z0r;
		coord_t zi = opts->z0i;
		
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
		for (int r=td->index; r<td->opts->height; r+=td->opts->threads) {
			if (td->flags & (THREAD_BEGIN | THREAD_EXIT)) {
				PRINT_THREAD_STATUS("interrupted!");
				break;
			} else {
				int y = td->opts->rowseq[r];
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
	for (int i=0; i<opts->threads; ++i) {
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
	png_set_crc_action(png, PNG_CRC_WARN_USE, PNG_CRC_WARN_USE);

	unsigned char sig[8];
	if (fread(sig, 1, 8, fp) != 8 || !png_check_sig(sig, 8)) {
		fprintf(stderr, "PNG signature not found; %s does not appear to be a PNG file.\n", filename);
		retval = 1;
	} else {
		png_set_sig_bytes(png, 8);
	}

	png_init_io(png, fp);
	png_read_png(png, info, 0, NULL);
	png_textp text;
	int count = png_get_text(png, info, &text, NULL);
    int iterations;
	coord_t z0r, z0i;
	int found = 0;
	for (int i=0; i<count; ++i) {
		if (!strcmp(text[i].key, FRACTAL_INFO_TEXT_KEY)) {
			found = 1;
			int scanf_result = sscanf(text[i].text, "%" CTFMT "g,%" CTFMT "g,%" CTFMT "g,%" CTFMT "g,%d,%" CTFMT "g,%" CTFMT "g", &view->xmin, &view->xmax, &view->ymin, &view->ymax, &iterations, &z0r, &z0i);
			if (scanf_result < 5) {
				fprintf(stderr, "Invalid " FRACTAL_INFO_TEXT_KEY " format in %s.\n", filename);
				retval = 1;
				goto init_from_png_exit;
			} else {
				if (opts->width == -1 && opts->height == -1) {
					opts->width = png_get_image_width(png, info);
					opts->height = png_get_image_height(png, info);
				}
				if (opts->iterations == -1) 
					opts->iterations = iterations;
				if (!(opts->flags & OPT_USER_Z0)) {
					opts->z0r = z0r;
					opts->z0i = z0i;
				}
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

#ifdef NO_LIBPNG
#define LIBPNG_USAGE ""
#else
#define LIBPNG_USAGE " [-r IMAGE.png]"
#endif

#ifdef USE_NPROCS
#define THREAD_OPT_HELP_DEFAULT_IS "the number of logical CPU cores in your PC (%d)"
#else
#define THREAD_OPT_HELP_DEFAULT_IS "%d"
#endif

void print_usage(FILE *fp, const char *argv0, int default_threads)
{
	fprintf(fp, "Usage: %s [-cP] [-w WIDTH] [-h HEIGHT] [-i ITERATIONS] [-p FILENAME] [-t THREADS] [-z re,im]" LIBPNG_USAGE "\n\n", argv0);
	fprintf(fp, " -w\tSets the width of the window. If absent, default size is %dx%d, or a 3:2 ratio with HEIGHT.\n", DEFAULT_WIDTH, DEFAULT_HEIGHT);
	fprintf(fp, " -h\tSets the height of the window. If absent, the same rules will be followed as with WIDTH.\n");
	fprintf(fp, " -i\tSets the number of iterations that will initially be used. Default is %d.\n", DEFAULT_ITER_COUNT);
	fprintf(fp, " -p\tLoads a palette from a file. The format is raw 8-bit RR GG BB [...]. Default is '" DEFAULT_PALETTE_FILENAME_1 "' or '" DEFAULT_PALETTE_FILENAME_2 "', if present.\n");
	fprintf(fp, " -P\tForces the use of the built-in (blue) palette, even if a palette exists with one of the default filenames.\n");
	fprintf(fp, " -c\tClear the window before redrawing.\n");
	fprintf(fp, " -t\tSets the number of threads to use. The default is " THREAD_OPT_HELP_DEFAULT_IS ".\n", default_threads);
	fprintf(fp, " -z\tSets a custom initial value of 'z' in the Mandelbrot equation. Default is 0,0, of course.\n");
#ifndef NO_LIBPNG
	fprintf(fp, " -r\tObtain parameters from a PNG image previously saved using the 'S' key. '-w', '-h', and '-i' take precedence.\n");
#endif
	fputc('\n', fp);
}

int main(int argc, char *argv[])
{
#ifdef TEST_PNG_ERROR
	fprintf(stderr, "WARNING: PNG export is deliberately broken in this build due to #define TEST_PNG_ERROR.\n");
#endif

	struct options opts;
	opts.flags = 0;
	opts.width = -1;
	opts.height = -1;
#ifdef USE_NPROCS
	opts.threads = get_nprocs_conf();
#else
	opts.threads = DEFAULT_THREADS;
#endif
	opts.z0r = opts.z0i = 0.0;
	
	if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
		print_usage(stdout, argv[0], opts.threads);
		return 0;
	}

	pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

	FILE *palette_file = fopen(DEFAULT_PALETTE_FILENAME_1, "rb");
	if (!palette_file) {
		palette_file = fopen(DEFAULT_PALETTE_FILENAME_2, "rb");
		/* If the open failed this time, we just need palette_file = NULL. So no further check is needed here. */
	}

	struct view_range view;
	init_options(&opts, &view);
	opts.iterations = -1;  /* in case there's a "-r" option given */

	int opt; while ((opt = getopt(argc, argv, "w:h:i:p:Pt:cz:" LIBPNG_GETOPT)) != -1) {
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
			if (palette_file)
				fclose(palette_file);
			palette_file = fopen(optarg, "rb");
			if (!palette_file) {
				perror(optarg);
				return 1;
			}
			break;
		case 'P':
			if (palette_file)
				fclose(palette_file);
			palette_file = NULL;
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
			break;
		case 'z':
			if (sscanf(optarg, "%" CTFMT "g,%" CTFMT "g", &opts.z0r, &opts.z0i) == 2) {
				opts.flags |= OPT_USER_Z0;
				break;
			} else {
				fprintf(stderr, "Invalid argument for '-z' option.\n");
				/* Fall through to case '?' */
			}
		case '?':
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return 255;
		}
	}
	if (opts.width == -1) {
		if (opts.height == -1) {
			opts.width = DEFAULT_WIDTH;
			opts.height = DEFAULT_HEIGHT;
		} else {
			opts.width = opts.height + opts.height / 2;
		}
	} else if (opts.height == -1) {
		if (opts.width == -1) {
			opts.width = DEFAULT_WIDTH;
			opts.height = DEFAULT_HEIGHT;
		} else {
			opts.height = opts.width - opts.width / 3;
		}
	}
	if (opts.iterations == -1)
		opts.iterations = DEFAULT_ITER_COUNT;
	if (opts.threads < 1 || opts.threads > opts.height) {
		fprintf(stderr, "%s: thread count must be between 1 and the current height (%d)\n", argv[0], opts.height);
		return 2;
	}

	opts.rowseq = calloc(opts.height, sizeof(int));
	int height_pwr2 = 1;
	int height_pwr2_bits = -1;
	while (height_pwr2 < opts.height) {
		height_pwr2 <<= 1;
		++height_pwr2_bits;
	}
	height_pwr2 >>= 1;
	int height_start = (opts.height - height_pwr2) / 2;
	for (int i=0; i<height_pwr2; ++i) {
		opts.rowseq[i] = height_start + reverse_bits(i, height_pwr2_bits);
	}
	for (int i=0; i<height_start; ++i) {
		opts.rowseq[height_pwr2 + 2*i] = i;
		opts.rowseq[height_pwr2 + 2*i + 1] = height_start + height_pwr2 + i;
	}

	struct thread_data *threads = calloc(opts.threads, sizeof(struct thread_data));
	for (int i=0; i<opts.threads; ++i) {
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
	for (int i=0; i<opts.threads; ++i) {
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

		for (int i=0; i<opts.palsize; ++i) {
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
							text[1].text_length = snprintf(textbuf, sizeof(textbuf), "%.20" CTFMT "g,%.20" CTFMT "g,%.20" CTFMT "g,%.20" CTFMT "g,%d,%.20" CTFMT "g,%.20" CTFMT "g", view.xmin, view.xmax, view.ymin, view.ymax, opts.iterations, opts.z0r, opts.z0i);
							text[1].text = textbuf;
							png_set_text(png, info, text, 2);

							pngsfc = SDL_CreateRGBSurface(0, opts.width, opts.height, 24, 0xFF, 0xFF00, 0xFF0000, 0);
							SDL_BlitSurface(sfc, NULL, pngsfc, NULL);
							pngrows = calloc(opts.height, sizeof(png_const_bytep));
							for (int i=0; i<opts.height; ++i) {
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
					printf("Xmin = % 2.20" CTFMT "f\n", view.xmin);
					printf("Xmax = % 2.20" CTFMT "f\n", view.xmax);
					printf("Ymin = % 2.20" CTFMT "f\n", view.ymin);
					printf("Ymax = % 2.20" CTFMT "f\n\n", view.ymax);
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

	for (int i=0; i<opts.threads; ++i) 
		threads[i].flags |= THREAD_EXIT;
	
	pthread_cond_broadcast(&cond);

	for (int i=0; i<opts.threads; ++i) {
		pthread_join(threads[i].thread, NULL);
		pthread_mutex_destroy(&threads[i].mutex);
	}
	
	pthread_cond_destroy(&cond);

	free(threads);
	free(opts.colormap);
	free(opts.rowseq);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
