/*
 * 86Box	A hypervisor and IBM PC system emulator that specializes in
 *		running old operating systems and software designed for IBM
 *		PC systems and compatibles from 1981 through fairly recent
 *		system designs based on the PCI bus.
 *
 *		This file is part of the 86Box distribution.
 *
 *		Renderers for ANSI text output in CLI mode.
 *
 *
 *
 * Author:	RichardG, <richardg867@gmail.com>
 *
 *		Copyright 2021 RichardG.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include <time.h>
#ifdef _WIN32
# include <windows.h>
# include <VersionHelpers.h>
#endif
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/mem.h>
#include <86box/timer.h>
#include <86box/keyboard_cli.h>
#include <86box/video.h>
#include <86box/vid_mda.h>
#include <86box/vid_cga.h>
#include <86box/vid_svga.h>
#include <86box/vid_text_render.h>


#define TEXT_RENDER_BUF_LINES	60
#define TEXT_RENDER_BUF_SIZE	4096	/* good for a fully packed SVGA 150-column row with some margin */

#define CHECK_INIT()	if (!text_render_initialized) \
				text_render_init();
#define APPEND_SGR()	if (!sgr_started) { \
				sgr_started = 1; \
				p += sprintf(p, "\033["); \
			} else \
				p += sprintf(p, ";");


enum {
    TERM_COLOR_NONE	= 0x00,
    TERM_COLOR_3BIT	= 0x01,
    TERM_COLOR_4BIT	= 0x03,
    TERM_COLOR_8BIT	= 0x07,
    TERM_COLOR_24BIT	= 0x0f
};

enum {
    /* SGR 6 provides a faster blink rate, more in line with IBM PC
       video cards, where supported. We can't enable both 5 and 6
       simultaneously, as they don't cancel each other out on mintty
       and possibly other terminals, resulting in irregular blinking. */
    TERM_CTL_RAPIDBLINK	= 0x01,
    /* Printing through aux port CSIs. */
    TERM_CTL_PRINT	= 0x02
};

enum {
    TERM_GFX_SIXEL	= 0x01,
    TERM_GFX_PNG	= 0x02,
    TERM_GFX_PNG_KITTY	= 0x04
};


/* Lookup table for converting CGA colors to the ANSI palette. */
const uint8_t ansi_palette[] = {
     0,  4,  2,  6,  1,  5,  3,  7, /* regular */
     8, 12, 10, 14,  9, 13, 11, 15  /* bright */
};

/* Lookup table for converting code page 437 to UTF-8. */
static const char *cp437[] = {
    /* 00 */ " ",            "\xE2\x98\xBA", "\xE2\x98\xBB", "\xE2\x99\xA5", "\xE2\x99\xA6", "\xE2\x99\xA3", "\xE2\x99\xA0", "\xE2\x80\xA2", "\xE2\x97\x98", "\xE2\x97\x8B", "\xE2\x97\x99", "\xE2\x99\x82", "\xE2\x99\x80", "\xE2\x99\xAA", "\xE2\x99\xAB", "\xE2\x98\xBC",
    /* 10 */ "\xE2\x96\xBA", "\xE2\x97\x84", "\xE2\x86\x95", "\xE2\x80\xBC", "\xC2\xB6",     "\xC2\xA7",     "\xE2\x96\xAC", "\xE2\x86\xA8", "\xE2\x86\x91", "\xE2\x86\x93", "\xE2\x86\x92", "\xE2\x86\x90", "\xE2\x88\x9F", "\xE2\x86\x94", "\xE2\x96\xB2", "\xE2\x96\xBC",
    /* 20 */ " ",            "!",            "\"",           "#",            "$",            "%",            "&",            "'",            "(",            ")",            "*",            "+",            ",",            "-",            ".",            "/",
    /* 30 */ "0",            "1",            "2",            "3",            "4",            "5",            "6",            "7",            "8",            "9",            ":",            ";",            "<",            "=",            ">",            "?",
    /* 40 */ "@",            "A",            "B",            "C",            "D",            "E",            "F",            "G",            "H",            "I",            "J",            "K",            "L",            "M",            "N",            "O",
    /* 50 */ "P",            "Q",            "R",            "S",            "T",            "U",            "V",            "W",            "X",            "Y",            "Z",            "[",            "\\",           "]",            "^",            "_",
    /* 60 */ "`",            "a",            "b",            "c",            "d",            "e",            "f",            "g",            "h",            "i",            "j",            "k",            "l",            "m",            "n",            "o",
    /* 70 */ "p",            "q",            "r",            "s",            "t",            "u",            "v",            "w",            "x",            "y",            "z",            "{",            "|",            "}",            "~",            "\xE2\x8C\x82",
    /* 80 */ "\xC3\x87",     "\xC3\xBC",     "\xC3\xA9",     "\xC3\xA2",     "\xC3\xA4",     "\xC3\xA0",     "\xC3\xA5",     "\xC3\xA7",     "\xC3\xAA",     "\xC3\xAB",     "\xC3\xA8",     "\xC3\xAF",     "\xC3\xAE",     "\xC3\xAC",     "\xC3\x84",     "\xC3\x85",
    /* 90 */ "\xC3\x89",     "\xC3\xA6",     "\xC3\x86",     "\xC3\xB4",     "\xC3\xB6",     "\xC3\xB2",     "\xC3\xBB",     "\xC3\xB9",     "\xC3\xBF",     "\xC3\x96",     "\xC3\x9C",     "\xC2\xA2",     "\xC2\xA3",     "\xC2\xA5",     "\xE2\x82\xA7", "\xC6\x92",
    /* A0 */ "\xC3\xA1",     "\xC3\xAD",     "\xC3\xB3",     "\xC3\xBA",     "\xC3\xB1",     "\xC3\x91",     "\xC2\xAA",     "\xC2\xBA",     "\xC2\xBF",     "\xE2\x8C\x90", "\xC2\xAC",     "\xC2\xBD",     "\xC2\xBC",     "\xC2\xA1",     "\xC2\xAB",     "\xC2\xBB",
    /* B0 */ "\xE2\x96\x91", "\xE2\x96\x92", "\xE2\x96\x93", "\xE2\x94\x82", "\xE2\x94\xA4", "\xE2\x95\xA1", "\xE2\x95\xA2", "\xE2\x95\x96", "\xE2\x95\x95", "\xE2\x95\xA3", "\xE2\x95\x91", "\xE2\x95\x97", "\xE2\x95\x9D", "\xE2\x95\x9C", "\xE2\x95\x9B", "\xE2\x94\x90",
    /* C0 */ "\xE2\x94\x94", "\xE2\x94\xB4", "\xE2\x94\xAC", "\xE2\x94\x9C", "\xE2\x94\x80", "\xE2\x94\xBC", "\xE2\x95\x9E", "\xE2\x95\x9F", "\xE2\x95\x9A", "\xE2\x95\x94", "\xE2\x95\xA9", "\xE2\x95\xA6", "\xE2\x95\xA0", "\xE2\x95\x90", "\xE2\x95\xAC", "\xE2\x95\xA7",
    /* D0 */ "\xE2\x95\xA8", "\xE2\x95\xA4", "\xE2\x95\xA5", "\xE2\x95\x99", "\xE2\x95\x98", "\xE2\x95\x92", "\xE2\x95\x93", "\xE2\x95\xAB", "\xE2\x95\xAA", "\xE2\x94\x98", "\xE2\x94\x8C", "\xE2\x96\x88", "\xE2\x96\x84", "\xE2\x96\x8C", "\xE2\x96\x90", "\xE2\x96\x80",
    /* E0 */ "\xCE\xB1",     "\xC3\x9F",     "\xCE\x93",     "\xCF\x80",     "\xCE\xA3",     "\xCF\x83",     "\xC2\xB5",     "\xCF\x84",     "\xCE\xA6",     "\xCE\x98",     "\xCE\xA9",     "\xCE\xB4",     "\xE2\x88\x9E", "\xCF\x86",     "\xCE\xB5",     "\xE2\x88\xA9",
    /* F0 */ "\xE2\x89\xA1", "\xC2\xB1",     "\xE2\x89\xA5", "\xE2\x89\xA4", "\xE2\x8C\xA0", "\xE2\x8C\xA1", "\xC3\xB7",     "\xE2\x89\x88", "\xC2\xB0",     "\xE2\x88\x99", "\xC2\xB7",     "\xE2\x88\x9A", "\xE2\x81\xBF", "\xC2\xB2",     "\xE2\x96\xA0", "\xC2\xA0"
};

/* Lookup table for encoding images as base64. */
static const char base64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Lookup table for terminal types. */
static const struct {
    const char	  *name;
    const uint8_t color;
    const uint8_t ctl;
    const uint8_t gfx;
} term_types[] = {
    {"iterm",		TERM_COLOR_24BIT, 0,					0			},
    {"iterm2",		TERM_COLOR_24BIT, 0,					TERM_GFX_PNG		},
    {"kitty",		TERM_COLOR_24BIT, 0,					TERM_GFX_PNG_KITTY	}, /* not to be confused with the PuTTY fork */
    {"konsole",		TERM_COLOR_24BIT, 0,					0			},
    {"linux",		TERM_COLOR_24BIT, 0,					0			},
    {"mintty",		TERM_COLOR_24BIT, TERM_CTL_RAPIDBLINK | TERM_CTL_PRINT,	TERM_GFX_SIXEL | TERM_GFX_PNG},
    {"termite",		TERM_COLOR_24BIT, 0,					0			}, /* not to be confused with the CompuPhase product */
    {"tmux",		TERM_COLOR_24BIT, 0,					0			},
    {"vte",		TERM_COLOR_24BIT, 0,					0			},
    {"xfce",		TERM_COLOR_24BIT, 0,					0			},
    {"xterm-24bit",	TERM_COLOR_24BIT, 0,					0			},
    {"xterm-24bits",	TERM_COLOR_24BIT, 0,					0			},
    {"xterm-256color",	TERM_COLOR_24BIT, 0,					0			},
    {"putty",		TERM_COLOR_4BIT,  0,					0			},
    {"xterm-16color",	TERM_COLOR_4BIT,  0,					0			},
    {"vt100",		TERM_COLOR_NONE,  0,					0			},
    {"vt220",		TERM_COLOR_NONE,  0,					0			},
    {"vt240",		TERM_COLOR_NONE,  0,					TERM_GFX_SIXEL		},
    {NULL, 0, 0, 0} /* assume TERM_COLOR_3BIT and no control/graphics capability for other terminals */
};


uint8_t text_render_initialized = 0,
	text_render_term_color = TERM_COLOR_3BIT, text_render_term_ctl = 0, text_render_term_gfx = 0,
	text_render_cx, text_render_cy;
uint32_t text_render_palette[16],
	 *text_render_8bitcol;
char	text_render_line_buffer[TEXT_RENDER_BUF_LINES][TEXT_RENDER_BUF_SIZE],
	text_render_gfx_str[32] = "\0";
int	text_render_gfx_w = -1, text_render_gfx_h = -1;
time_t	text_render_gfx_last = 0;


/* Callbacks specific to each color capability level. */
int (*text_render_setcolor)(char *p, uint8_t idx, uint8_t bg);
void (*text_render_setpal)(uint8_t index, uint32_t color) = text_render_setpal_init;


int
text_render_setcolor_noop(char *p, uint8_t idx, uint8_t bg)
{
    /* No color support. */
    return 0;
}

int
text_render_setcolor_3bit(char *p, uint8_t idx, uint8_t bg)
{
    /* Set a color from the 8-color ANSI palette, ignoring the bright bit. */
    return sprintf(p, "%d", (bg ? 40 : 30) + (idx & 7));
}

int
text_render_setcolor_4bit(char *p, uint8_t idx, uint8_t bg) {
    /* Set a color from the 16-color ANSI palette. */
    uint8_t pre_attr = 0, sgr = (bg ? 40 : 30) + (idx & 7);

    if (idx & 8) {
	if (bg)
		sgr += 60; /* bright background: use non-standard SGR */
	else
		pre_attr = 1; /* bright foreground: increase intensity */
    } else if (!bg)
	pre_attr = 22; /* regular foreground: decrease intensity */

    if (pre_attr)
	return sprintf(p, "%d;%d", pre_attr, sgr);
    else
	return sprintf(p, "%d", sgr);
}

int
text_render_setcolor_8bit(char *p, uint8_t idx, uint8_t bg)
{
    /* Set a color from the 256-color palette using the approximations calculated by text_render_setpal_8bit */
    uint8_t approx = text_render_palette[idx];
    if (approx < 8) /* save a little bit of bandwidth by using standard SGRs on 8-color palette colors */
	return sprintf(p, "%d", (bg ? 40 : 30) + approx);
    else
	return sprintf(p, "%d;5;%d", bg ? 48 : 38, approx);
}

int
text_render_setcolor_24bit(char *p, uint8_t idx, uint8_t bg)
{
    /* Set a full RGB color. */
    uint32_t color = text_render_palette[idx];
    return sprintf(p, "%d;2;%d;%d;%d", bg ? 48 : 38, (color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff);
}

void
text_render_setpal_noop(uint8_t index, uint32_t color)
{
    /* No palette mapping on low-color terminals. */
}

void
text_render_setpal_8bit(uint8_t index, uint32_t color)
{
    /* Look through the 256-color palette for the closest color to the desired one. */
    uint32_t palette_color;
    uint8_t best_idx = 0, rdif, gdif, bdif;
    double candidate, best = INFINITY;

    for (int i = 0; i < 256; i++) {
	palette_color = text_render_8bitcol[i];
	if (palette_color == color) {
		/* Stop immediately if an exact match was found. */
		best_idx = i;
		break;
	}

	rdif = ((palette_color >> 16) & 0xff) - ((color >> 16) & 0xff);
	gdif = ((palette_color >> 8) & 0xff) - ((color >> 8) & 0xff);
	bdif = (palette_color & 0xff) - (color & 0xff);

	/* Controversial formula, but good enough? */
	candidate = sqrt((rdif * rdif) + (gdif * gdif) + (bdif * bdif));
	if (candidate < best) {
		best = candidate;
		best_idx = i;
	}
    }

    /* Store the 256-color value in our local palette. */
    text_render_palette[index] = best_idx;
}

void
text_render_setpal_24bit(uint8_t index, uint32_t color)
{
    /* True color terminals can be given the full RGB color. */
    text_render_palette[index] = color;
}

void
text_render_setpal_init(uint8_t index, uint32_t color)
{
    /* Initialize the text renderer, then retry. */
    CHECK_INIT();
    text_render_setpal(index, color);
}


void
text_render_updatecursor()
{
    if ((text_render_cx == ((uint8_t) -1)) || (text_render_cy == ((uint8_t) -1))) /* cursor disabled */
	fprintf(TEXT_RENDER_OUTPUT, "\033[?25l");
    else /* cursor enabled */
	fprintf(TEXT_RENDER_OUTPUT, "\033[%d;%dH\033[?25h", text_render_cy + 1, text_render_cx + 1);
    fflush(TEXT_RENDER_OUTPUT);
}

int
text_render_detectterm(char *env) {
    if (!env)
	return -1;

    for (int i = 0; term_types[i].name; i++) {
	if (!strcasecmp(env, term_types[i].name))
		return i;
    }

    return -1;
}

void
text_render_fillcolortable(uint32_t *table, uint16_t count)
{
    /* Fill a color table with up to a 256-color palette.
       Algorithm from Linux's vt.c */
    uint32_t color;
    for (uint16_t i = 0; i < count; i++) {
	if (i < 8) { /* 8-color ANSI */
		color = 0;
		if (i & 1)
			color |= 0xaa0000;
		if (i & 2)
			color |= 0x00aa00;
		if (i & 4)
			color |= 0x0000aa;
	} else if (i < 16) { /* 16-color ANSI (bright) */
		color = 0x555555;
		if (i & 1)
			color |= 0xff0000;
		if (i & 2)
			color |= 0x00ff00;
		if (i & 4)
			color |= 0x0000ff;
	} else if (i < 232) { /* color cube */
		color  = (uint8_t) ((i - 16) / 36 * 85 / 2) << 16;
		color |= (uint8_t) ((i - 16) / 6 % 6 * 85 / 2) << 8;
		color |= (uint8_t) ((i - 16) % 6 * 85 / 2);
	} else { /* grayscale ramp */
		color  = (uint8_t) (i * 10 - 2312);
		color |= color << 8;
		color |= (color & 0xff) << 16;
	}
	table[i] = color;
    }
}

void
text_render_init()
{
    char *env;
    int i;

    text_render_initialized = 1;

    /* Initialize keyboard input. */
    keyboard_cli_init();

    /* Detect this terminal's capabilities. */
    i = text_render_detectterm(getenv("TERM_PROGRAM"));
    if (i == -1)
	i = text_render_detectterm(getenv("TERM"));

    if (i != -1) {
	text_render_term_color = term_types[i].color;
	text_render_term_ctl = term_types[i].ctl;
	text_render_term_gfx = term_types[i].gfx;
    }
#ifdef _WIN32
    else {
	/* Assume an unknown terminal on Windows to be cmd. */
	if (IsWindows10OrGreater()) /* true color is supported on Windows 10 */
		text_render_term_color = TERM_COLOR_24BIT;
	else /* older versions are limited to 16 colors */
		text_render_term_color = TERM_COLOR_4BIT;
    }
#endif

    /* Detect a true color terminal through the COLORTERM environment variable. */
    if (text_render_term_color < TERM_COLOR_24BIT) {
	env = getenv("COLORTERM");
	if (env && (strcasecmp(env, "truecolor") || strcasecmp(env, "24bit")))
		text_render_term_color = TERM_COLOR_24BIT;

	/* TODO: once keyboard input is figured out, there is a third method
	   to detect a true color terminal, through a DECRQSS query.
	   ref: https://gist.github.com/XVilka/8346728 */
    }

    /* Initialize palette tables for high-color terminals. */
    if (text_render_term_color >= TERM_COLOR_24BIT) {
	/* Fill standard ANSI color values. */
	text_render_fillcolortable(text_render_palette, sizeof(text_render_palette) / sizeof(text_render_palette[0]));
    } else if (text_render_term_color >= TERM_COLOR_8BIT) {
	/* Map to ANSI colors at first. */
	for (i = 0; i < sizeof(text_render_palette) / sizeof(text_render_palette[0]); i++)
		text_render_palette[i] = i;

	/* Fill color values. */
	text_render_8bitcol = malloc(256 * sizeof(uint32_t));
	text_render_fillcolortable(text_render_8bitcol, 256);
    }

    /* Set the correct setcolor function for this terminal's color capability. */
    switch (text_render_term_color) {
	case TERM_COLOR_3BIT:
		text_render_setcolor = text_render_setcolor_3bit;
		text_render_setpal = text_render_setpal_noop;
		break;

	case TERM_COLOR_4BIT:
		text_render_setcolor = text_render_setcolor_4bit;
		text_render_setpal = text_render_setpal_noop;
		break;

	case TERM_COLOR_8BIT:
		text_render_setcolor = text_render_setcolor_8bit;
		text_render_setpal = text_render_setpal_8bit;
		break;

	case TERM_COLOR_24BIT:
		text_render_setcolor = text_render_setcolor_24bit;
		text_render_setpal = text_render_setpal_24bit;
		break;

	default:
		text_render_setcolor = text_render_setcolor_noop;
		text_render_setpal = text_render_setpal_noop;
		break;
    }

    /* Override the default color for dark yellow, as CGA typically renders that as brown. */
    text_render_setpal(3, 0xaa5500);

    /* Start with the cursor disabled. */
    text_render_cx = -1;
    text_render_updatecursor();
}


void
text_render_blank()
{
    CHECK_INIT();

    char *buf, *p;

    /* Clear screen if we're not rendering the graphics mode box. */
    if (text_render_line_buffer[0][0] != '\xFF') {
	p = buf = malloc(256);
	p += sprintf(p, "\033[");
	p += text_render_setcolor(p, 0, 1);
	fprintf(TEXT_RENDER_OUTPUT, "%sm\033[2J\033[3J", buf);
	free(buf);
    }

    /* Disable cursor and flush output. */
    text_render_cx = -1;
    text_render_updatecursor();
}

void
text_render_gfx(char *str)
{
    /* Let video.c trigger an image render if this terminal supports graphics. */
    if (text_render_term_gfx & (TERM_GFX_PNG | TERM_GFX_PNG_KITTY)) {
	if (time(NULL) - text_render_gfx_last) /* render at 1 FPS */
		text_render_png = 1;
	return;
    }

    /* Render infobox otherwise. */
    text_render_gfx_box(str);
}

void
text_render_gfx_box(char *str)
{
    CHECK_INIT();

    uint8_t render = 0, i, w, h;
    char buf[256];
    const char boxattr[] = "\033[30;47m", resetattr[] = "\033[0m";

    /* Render only if the width, height or format string changed. */
    w = get_actual_size_x();
    if (w != text_render_gfx_w) {
	render = 1;
	text_render_gfx_w = w;
    }

    h = get_actual_size_y();
    if (h != text_render_gfx_h) {
	render = 1;
	text_render_gfx_h = h;
    }

    if (!strncmp(str, text_render_gfx_str, sizeof(text_render_gfx_str) - 1)) {
	render = 1;
	strncpy(text_render_gfx_str, str, sizeof(text_render_gfx_str) - 1);
    }

    if (render) {
	/* Clear the screen if this is the first time we're rendering this. */
	if (text_render_line_buffer[0][0] != '\xFF') {
		fprintf(TEXT_RENDER_OUTPUT, "\033[2J\033[3J");

		/* Force invalidation of the first 3 line buffers, such that this renderer's output
		   is guaranteed to be overwritten whenever a proper text renderer comes back online. */
		for (i = 0; i < 3; i++)
			strcpy(text_render_line_buffer[i], "\xFF"); /* invalid UTF-8 */
	}

	/* Print message enclosed in a box. */
	sprintf(buf, str, get_actual_size_x(), get_actual_size_y());
	fprintf(TEXT_RENDER_OUTPUT, "\033[1;1H%s%s%s", resetattr, boxattr, cp437[0xc9]);
	for (i = 0; i < strlen(buf); i++)
		fprintf(TEXT_RENDER_OUTPUT, cp437[0xcd]);
	fprintf(TEXT_RENDER_OUTPUT, "%s%s\033[2;1H%s%s%s%s%s\033[3;1H%s%s",
		cp437[0xbb], resetattr,
		boxattr, cp437[0xba], buf, cp437[0xba], resetattr,
		boxattr, cp437[0xc8]);
	for (i = 0; i < strlen(buf); i++)
		fprintf(TEXT_RENDER_OUTPUT, cp437[0xcd]);
	fprintf(TEXT_RENDER_OUTPUT, "%s%s", cp437[0xbc], resetattr);

	/* Disable cursor and flush output. */
	text_render_cx = -1;
	text_render_updatecursor();
    }
}

static void
base64_encode_tri(uint8_t *buf, uint8_t len)
{
    uint32_t tri = buf[0] << 16;
    if (len >= 2) {
	tri |= buf[1] << 8;
	if (len == 3)
		tri |= buf[2];
    }
    fprintf(TEXT_RENDER_OUTPUT, "%c%c%c%c",
	    base64[tri >> 18],
	    base64[(tri >> 12) & 0x3f],
	    (len == 1) ? '=' : base64[(tri >> 6) & 0x3f],
	    (len <= 2) ? '=' : base64[tri & 0x3f]);
}

void
text_render_gfx_image(char *fn)
{
    /* Open image file. */
    FILE *f = fopen(fn, "rb");
    if (!f)
	return;

    /* Determine the image file's size. */
    int size = fseek(f, 0, SEEK_END) + 1;
    if (size < 1)
	goto end;
    fseek(f, 0, SEEK_SET);

    /* Invalidate any infobox contents. */
    text_render_gfx_w = -1;
    text_render_line_buffer[0][0] = '\x00';

    /* Move to the top left corner. */
    fprintf(TEXT_RENDER_OUTPUT, "\033[1;1H");

    /* Output image according to the terminal's capabilities. */
    uint8_t buf[3], read;
    if (text_render_term_gfx & TERM_GFX_PNG) {
	/* Output header. */
	fprintf(TEXT_RENDER_OUTPUT, "\033]1337;File=name=cy5wbmc=;size=%d:", size);

	/* Encode the image as base64. */
	while ((read = fread(buf, 1, 3, f)))
		base64_encode_tri(buf, read);

	/* Output terminator. */
	fprintf(TEXT_RENDER_OUTPUT, "\a");
    } else if (text_render_term_gfx & TERM_GFX_PNG_KITTY) {
	uint8_t first = 1;
	while (size) {
		/* Output chunk header. */
		fprintf(TEXT_RENDER_OUTPUT, "\033_G");
		if (first) {
			first = 0;
			fprintf(TEXT_RENDER_OUTPUT, "f=100,");
		}
		fprintf(TEXT_RENDER_OUTPUT, "m=%d;", size > 4096);

		/* Output up to 4096 bytes (1024 base64 encoded quads) per chunk. */
		for (int i = 0; i < 1024; i++) {
			if ((read = fread(buf, 1, 3, f)))
				base64_encode_tri(buf, read);
			else
				break;
		}

		/* Output chunk terminator. */
		fprintf(TEXT_RENDER_OUTPUT, "\033\\");
	}
    }

end:
    /* Clean up. */
    fflush(TEXT_RENDER_OUTPUT);
    fclose(f);

    /* Set last render time to keep track of framerate. */
    text_render_gfx_last = time(NULL);
}

void
text_render_mda(uint8_t xlimit,
		uint8_t *fb, uint16_t fb_base,
		uint8_t do_render, uint8_t do_blink,
		uint16_t ca, uint8_t con)
{
    CHECK_INIT();

    char buf[TEXT_RENDER_BUF_SIZE], *p;
    int x;
    uint32_t ma = fb_base;
    uint8_t chr, attr, attr77, cy = ma / xlimit, cx = 0, new_cx = text_render_cx, new_cy = text_render_cy;
    uint8_t sgr_started = 0, sgr_blackout = 1, sgr_ul, sgr_int, sgr_blink, sgr_reverse;
    uint8_t prev_sgr_ul = 0, prev_sgr_int = 0, prev_sgr_blink = 0, prev_sgr_reverse = 0;

    /* Don't overflow the framebuffer. */
    if (cy >= TEXT_RENDER_BUF_LINES)
	return;

    p = buf;
    p += sprintf(p, "\033[%d;1H", cy + 1);
    if (text_render_setcolor != text_render_setcolor_noop) {
	p += sprintf(p, "\033[");
	p += text_render_setcolor(p, 0, 1);
	p += sprintf(p, "m");
    }
    p += sprintf(p, "\033[2K");

    for (x = 0; x < xlimit; x++) {
	if (do_render) {
		chr  = fb[(ma << 1) & 0xfff];
		attr = fb[((ma << 1) + 1) & 0xfff];
	} else
		chr = attr = 0;

	attr77 = attr & 0x77;
	if (!attr77) {
		/* Create a blank space by discarding all attributes then printing a space. */
		if (!sgr_blackout) {
			APPEND_SGR();
			p += sprintf(p, "0");
			sgr_blackout = 1;
			prev_sgr_ul = prev_sgr_int = prev_sgr_blink = prev_sgr_reverse = 0;
		}
		chr = 0;
	} else {
		sgr_blackout = 0;

		/* Set reverse. */
		sgr_reverse = (attr77 == 0x70);
		if (sgr_reverse != prev_sgr_reverse) {
			APPEND_SGR();
			p += sprintf(p, sgr_reverse ? "7" : "27");
			prev_sgr_reverse = sgr_reverse;
		}

		/* Set underline. Cannot co-exist with reverse. */
		sgr_ul = ((attr & 0x07) == 1) && !sgr_reverse;
		if (sgr_ul != prev_sgr_ul) {
			APPEND_SGR();
			p += sprintf(p, sgr_ul ? "4" : "24");
			prev_sgr_ul = sgr_ul;
		}

		/* Set blink, if enabled. */
		sgr_blink = (attr & 0x80) && do_blink;
		if (sgr_blink != prev_sgr_blink) {
			APPEND_SGR();
			p += sprintf(p, sgr_blink ? ((text_render_term_ctl & TERM_CTL_RAPIDBLINK) ? "6" : "5") : "25");
			prev_sgr_blink = sgr_blink;
		}

		/* Set intense. Cannot co-exist with both reverse and blink simultaneously. */
		sgr_int = (attr & 0x08) && !(sgr_reverse && sgr_blink);
		if (sgr_int != prev_sgr_int) {
			APPEND_SGR();
			p += sprintf(p, sgr_int ? "1" : "22");
			prev_sgr_int = sgr_int;
		}
	}

	/* If the cursor is on this location, set it as the new cursor position. */
	if ((ma == ca) && con) {
		new_cx = cx;
		new_cy = cy;
	}

	/* Finish any SGRs we may have started. */
	if (sgr_started) {
		sgr_started = 0;
		p += sprintf(p, "m");
	}

	/* Output the character. */
	p += sprintf(p, cp437[chr]);

	ma++;
	cx++;
    }

    if (memcmp(buf, text_render_line_buffer[cy], p - buf)) {
	fprintf(TEXT_RENDER_OUTPUT, buf);
	text_render_cx = ~new_cx; /* force a cursor update, which also flushes the output */
	memcpy(text_render_line_buffer[cy], buf, p - buf);
    }

    /* Update cursor if required. */
    if ((new_cx != text_render_cx) || (new_cy != text_render_cy)) {
	if (con) {
		text_render_cx = new_cx;
		text_render_cy = new_cy;
	} else {
		text_render_cx = -1;
	}
	text_render_updatecursor();
    }
}

void
text_render_cga(uint8_t cy,
		int xlimit, int xinc,
		uint8_t *fb, uint32_t fb_base, uint32_t fb_mask, uint8_t fb_step,
		uint8_t do_render, uint8_t do_blink,
		uint32_t ca, uint8_t con)
{
    CHECK_INIT();

    char buf[TEXT_RENDER_BUF_SIZE], *p;
    int x;
    uint32_t ma = fb_base;
    uint8_t chr, attr, cx = 0, new_cx = text_render_cx, new_cy = text_render_cy;
    uint8_t sgr_started = 0, sgr_bg, sgr_fg, sgr_blink;
    uint8_t prev_sgr_bg = 0, prev_sgr_fg = 0, prev_sgr_blink = 0;

    /* Don't overflow the framebuffer. */
    if (cy >= TEXT_RENDER_BUF_LINES)
	return;

    p = buf;
    p += sprintf(p, "\033[%d;1H", cy + 1);
    if (text_render_setcolor != text_render_setcolor_noop) {
	p += sprintf(p, "\033[");
	p += text_render_setcolor(p, 0, 1);
	p += sprintf(p, "m");
    }
    p += sprintf(p, "\033[2K");

#if 0
    fb[((ma << 1) + (fb_step * 0)) & fb_mask] = '0' + (cy / 10);
    fb[((ma << 1) + (fb_step * 1)) & fb_mask] = 0x0f;
    fb[((ma << 1) + (fb_step * 2)) & fb_mask] = '0' + (cy % 10);
    fb[((ma << 1) + (fb_step * 3)) & fb_mask] = 0x0f;
#endif

    for (x = 0; x < xlimit; x += xinc) {
	if (do_render) {
		chr  = fb[(ma << 1) & fb_mask];
		attr = fb[((ma << 1) + 1) & fb_mask];
	} else
		chr = attr = 0;

	/* Set foreground color. */
	sgr_fg = ansi_palette[attr & 15];
	if ((x == 0) || (sgr_fg != prev_sgr_fg)) {
		APPEND_SGR();
		p += text_render_setcolor(p, sgr_fg, 0);
		prev_sgr_fg = sgr_fg;
	}

	/* If blinking is enabled, use the top bit for that instead of bright background. */
	if (do_blink) {
		sgr_blink = attr & 0x80;
		attr &= 0x7f;
	} else
		sgr_blink = 0;

	/* Set background color. */
	sgr_bg = ansi_palette[attr >> 4];
	if ((x == 0) || (sgr_bg != prev_sgr_bg)) {
		APPEND_SGR();
		p += text_render_setcolor(p, sgr_bg, 1);
		prev_sgr_bg = sgr_bg;
	}

	/* Set blink. */
	if ((x == 0) || (sgr_blink != prev_sgr_blink)) {
		APPEND_SGR();
		p += sprintf(p, sgr_blink ? ((text_render_term_ctl & TERM_CTL_RAPIDBLINK) ? "6" : "5") : "25");
		prev_sgr_blink = sgr_blink;
	}

	/* If the cursor is on this location, set it as the new cursor position. */
	if ((ma == ca) && con) {
		new_cx = cx;
		new_cy = cy;
	}

	/* Finish any SGRs we may have started. */
	if (sgr_started) {
		sgr_started = 0;
		p += sprintf(p, "m");
	}

	/* Output the character. */
	p += sprintf(p, cp437[chr]);

	ma += fb_step;
	cx++;
    }

    if (memcmp(buf, text_render_line_buffer[cy], p - buf)) {
	fprintf(TEXT_RENDER_OUTPUT, buf);
	text_render_cx = ~new_cx; /* force a cursor update, which also flushes the output */
	memcpy(text_render_line_buffer[cy], buf, p - buf);
    }

    /* Update cursor if required. */
    if ((new_cx != text_render_cx) || (new_cy != text_render_cy)) {
	if (con) {
		text_render_cx = new_cx;
		text_render_cy = new_cy;
	} else {
		text_render_cx = -1;
	}
	text_render_updatecursor();
    }
}
