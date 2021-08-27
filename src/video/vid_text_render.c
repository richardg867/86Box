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
#include <signal.h>
#include <time.h>
#ifdef _WIN32
# include <windows.h>
# include <VersionHelpers.h>
#endif
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/mem.h>
#include <86box/plat.h>
#include <86box/timer.h>
#include <86box/keyboard_cli.h>
#include <86box/version.h>
#include <86box/video.h>
#include <86box/vid_mda.h>
#include <86box/vid_cga.h>
#include <86box/vid_svga.h>
#include <86box/vid_text_render.h>


#define TEXT_RENDER_BUF_LINES	60
#define TEXT_RENDER_BUF_SIZE_FB	150 * 2
#define TEXT_RENDER_BUF_SIZE	4096	/* good for a fully packed SVGA 150-column row with some margin */

#define CHECK_INIT()	if (!cli_initialized) \
				text_render_init();
#define APPEND_SGR()	if (!sgr_started) { \
				sgr_started = 1; \
				p += sprintf(p, "\033["); \
			} else { \
				p += sprintf(p, ";"); \
			}


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

/* Entries for the CLI menu. */
static const char *menu_entries[] = {
    "[Enter] Go back to machine",
    "[R] Hard reset",
    "[Del] Send Ctrl+Alt+Del",
    "[E] Send Ctrl+Alt+Esc",
    "[S] Take screenshot",
    "[P] Pause",
    "[Q] Exit"
};


struct {
    thread_t	*thread;
    event_t	*wake_render_thread, *render_complete;
    char	*output;
    uint8_t	*fb, color, y, do_render, do_blink, con;
    uint16_t	ca;
    uint32_t	fb_base, fb_mask, fb_step;
    int		xlimit, xinc;
} render_data;

uint8_t		cli_initialized = 0, cli_menu = 0;
static uint8_t	term_color = TERM_COLOR_3BIT, term_ctl = 0, term_gfx = 0,
		term_sx = 80, term_sy = 24, /* terminals are 24 by default, not 25 */
		cursor_x, cursor_y,
		menu_max_width = 0;
static uint16_t	line_framebuffer[TEXT_RENDER_BUF_LINES][TEXT_RENDER_BUF_SIZE_FB];
static uint32_t	color_palette[16], *color_palette_8bit;
static char	line_buffer[TEXT_RENDER_BUF_LINES][TEXT_RENDER_BUF_SIZE],
		gfx_str[32] = "\0";
static int	gfx_w = -1, gfx_h = -1;
static time_t	gfx_last = 0;

/* Callbacks specific to each color capability level. */
int (*text_render_setcolor)(char *p, uint8_t idx, uint8_t bg);
void (*text_render_setpal)(uint8_t index, uint32_t color) = text_render_setpal_init;


static void	cli_render_process();


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
    uint8_t approx = color_palette[idx];
    if (approx < 8) /* save a little bit of bandwidth by using standard SGRs on 8-color palette colors */
	return sprintf(p, "%d", (bg ? 40 : 30) + approx);
    else
	return sprintf(p, "%d;5;%d", bg ? 48 : 38, approx);
}

int
text_render_setcolor_24bit(char *p, uint8_t idx, uint8_t bg)
{
    /* Set a full RGB color. */
    uint32_t color = color_palette[idx];
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
	palette_color = color_palette_8bit[i];
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
    color_palette[index] = best_idx;
}

void
text_render_setpal_24bit(uint8_t index, uint32_t color)
{
    /* True color terminals can be given the full RGB color. */
    color_palette[index] = color;
}

void
text_render_setpal_init(uint8_t index, uint32_t color)
{
    /* Initialize the text renderer, then retry. */
    CHECK_INIT();
    text_render_setpal(index, color);
}


static void
text_render_updateline(char *buf, uint8_t y, uint8_t new_cx, uint8_t new_cy)
{
    /* Update line if required and within the terminal's limit. */
    if ((!buf || strcmp(buf, line_buffer[y])) && (y < term_sy)) {
	/* Move to line and reset formatting. */
	fprintf(TEXT_RENDER_OUTPUT, "\033[%d;1H", y + 1);
	if (text_render_setcolor != text_render_setcolor_noop) {
		char sgr[256] = "\033[0;";
		if (!text_render_setcolor(&sgr[4], 0, 1))
			sgr[3] = '\0';
		fprintf(TEXT_RENDER_OUTPUT, "%sm", sgr);
	}
	fprintf(TEXT_RENDER_OUTPUT, "\033[2K");

	if (buf) {
		/* Copy line to buffer. */
		strcpy(line_buffer[y], buf);
	}

	/* Print line. */
	fprintf(TEXT_RENDER_OUTPUT, line_buffer[y]);

	/* Force cursor update. */
	cursor_x = ~new_cx;
    }

    /* Update cursor if required. */
    if ((new_cx != cursor_x) || (new_cy != cursor_y)) {
	cursor_x = new_cx;
	cursor_y = new_cy;

	if ((cursor_x == ((uint8_t) -1)) || (cursor_x >= term_sx) ||
	    (cursor_y == ((uint8_t) -1)) || (cursor_y >= term_sy)) { /* cursor disabled */
		pclog("cursor disabled\n"); fprintf(TEXT_RENDER_OUTPUT, "\033[?25l"); }
	else /* cursor enabled */ {
		pclog("cursor enabled at %d:%d\n", cursor_x, cursor_y);
		fprintf(TEXT_RENDER_OUTPUT, "\033[%d;%dH\033[?25h", cursor_y + 1, cursor_x + 1);
	}
    }

    /* Flush output. */
    fflush(TEXT_RENDER_OUTPUT);
}


static void
text_render_updatescreen(int sig)
{
    /* Force a redraw on each line. */
    for (int i = 0; i < TEXT_RENDER_BUF_LINES; i++)
	text_render_updateline(NULL, i, cursor_x, cursor_y);
}


static int
text_render_detectterm(char *env) {
    if (!env)
	return -1;

    for (int i = 0; term_types[i].name; i++) {
	if (!strcasecmp(env, term_types[i].name))
		return i;
    }

    return -1;
}


static void
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
		color |= color << 8;
	}
	table[i] = color;
    }
}

void
text_render_init()
{
    char *env;
    int i, j;

    cli_initialized = 1;

    /* Initialize keyboard input. */
    keyboard_cli_init();

    /* Detect this terminal's capabilities. */
    i = text_render_detectterm(getenv("TERM_PROGRAM"));
    if (i == -1)
	i = text_render_detectterm(getenv("TERM"));

    if (i != -1) {
	term_color = term_types[i].color;
	term_ctl = term_types[i].ctl;
	term_gfx = term_types[i].gfx;
    }
#ifdef _WIN32
    else {
	/* Assume an unknown terminal on Windows to be cmd. */
	if (IsWindows10OrGreater()) /* true color is supported on Windows 10 */
		term_color = TERM_COLOR_24BIT;
	else /* older versions are limited to 16 colors */
		term_color = TERM_COLOR_4BIT;
    }
#endif

    /* Detect this terminal's color capability through the
       COLORTERM environment variable or DECRQSS SGR queries. */
    if (term_color < TERM_COLOR_24BIT) {
	env = getenv("COLORTERM");
	if (env && (strcasecmp(env, "truecolor") || strcasecmp(env, "24bit"))) {
		term_color = TERM_COLOR_24BIT; 
	} else {
		fprintf(TEXT_RENDER_OUTPUT, "\033[38;2;1;2;3m");
		if (keyboard_cli_decrqss_str("$qm", "38:2:1:2:3") >= 0) {
			term_color = TERM_COLOR_24BIT;
		} else {
			fprintf(TEXT_RENDER_OUTPUT, "\033[38;5;255m");
			if (keyboard_cli_decrqss_str("$qm", "38:5:255") >= 0) {
				term_color = TERM_COLOR_8BIT;
			} else {
				fprintf(TEXT_RENDER_OUTPUT, "\033[97m");
				if (keyboard_cli_decrqss_str("$qm", "97") >= 0)
					term_color = TERM_COLOR_4BIT;
			}
		}
		fprintf(TEXT_RENDER_OUTPUT, "\033[0m");
	}
    }

    /* Initialize palette tables for high-color terminals. */
    if (term_color >= TERM_COLOR_24BIT) {
	/* Fill standard ANSI color values. */
	text_render_fillcolortable(color_palette, sizeof(color_palette) / sizeof(color_palette[0]));
    } else if (term_color >= TERM_COLOR_8BIT) {
	/* Map to ANSI colors at first. */
	for (i = 0; i < sizeof(color_palette) / sizeof(color_palette[0]); i++)
		color_palette[i] = i;

	/* Fill color values. */
	color_palette_8bit = malloc(256 * sizeof(uint32_t));
	text_render_fillcolortable(color_palette_8bit, 256);
    }

    /* Set the correct setcolor function for this terminal's color capability. */
    switch (term_color) {
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

    /* Start render thread. */
    render_data.thread = thread_create(cli_render_process, NULL);
    render_data.wake_render_thread = thread_create_event();
    render_data.render_complete = thread_create_event();
    thread_set_event(render_data.render_complete);

#ifdef SIGWINCH
    /* Redraw screen on terminal resize. */
    signal(SIGWINCH, text_render_updatescreen);
#endif

    /* Determine the longest menu entry. */
    for (i = 0; i < (sizeof(menu_entries) / sizeof(menu_entries[0])); i++) {
	j = strlen(menu_entries[i]);
	if (j > menu_max_width)
		menu_max_width = j;
    }
}


void
text_render_blank()
{
    CHECK_INIT();
#if 0
    char *buf, *p;

    /* Clear screen if we're not rendering the graphics mode box. */
    if (line_buffer[0][0] != '\xFF') {
	p = buf = malloc(256);
	p += sprintf(p, "\033[");
	p += text_render_setcolor(p, 0, 1);
	fprintf(TEXT_RENDER_OUTPUT, "%sm\033[2J\033[3J", buf);
	free(buf);
    }

    /* Disable cursor and flush output. */
    cursor_x = -1;
    for (int i = 0; i < term_sy; i++)
	text_render_updateline("\0", i, cursor_x, cursor_y);
#endif
}


void
text_render_gfx(char *str)
{
    /* Let video.c trigger an image render if this terminal supports graphics. */
    if (term_gfx & (TERM_GFX_PNG | TERM_GFX_PNG_KITTY)) {
	if (time(NULL) - gfx_last) /* render at 1 FPS */
		text_render_png = 1;
	return;
    }

    /* Render infobox otherwise. */
    //text_render_gfx_box(str);
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
    if (w != gfx_w) {
	render = 1;
	gfx_w = w;
    }

    h = get_actual_size_y();
    if (h != gfx_h) {
	render = 1;
	gfx_h = h;
    }

    if (!strncmp(str, gfx_str, sizeof(gfx_str) - 1)) {
	render = 1;
	strncpy(gfx_str, str, sizeof(gfx_str) - 1);
    }

    if (render) {
	/* Clear the screen if this is the first time we're rendering this. */
	if (line_buffer[0][0] != '\xFF') {
		fprintf(TEXT_RENDER_OUTPUT, "\033[2J\033[3J");

		/* Force invalidation of the first 3 line buffers, such that this renderer's output
		   is guaranteed to be overwritten whenever a proper text renderer comes back online. */
		for (i = 0; i < 3; i++)
			strcpy(line_buffer[i], "\xFF"); /* invalid UTF-8 */
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
	cursor_x = -1;
    }

    for (int i = 0; i < term_sy; i++)
	text_render_updateline("", i, cursor_x, cursor_y);
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
    gfx_w = -1;
    line_buffer[0][0] = '\x00';

    /* Move to the top left corner. */
    fprintf(TEXT_RENDER_OUTPUT, "\033[1;1H");

    /* Output image according to the terminal's capabilities. */
    uint8_t buf[3], read;
    int i;
    if (term_gfx & TERM_GFX_PNG) {
	/* Output header. */
	fprintf(TEXT_RENDER_OUTPUT, "\033]1337;File=name=cy5wbmc=;size=%d:", size);

	/* Output image as base64. */
	while ((read = fread(buf, 1, 3, f)))
		base64_encode_tri(buf, read);

	/* Output terminator. */
	fprintf(TEXT_RENDER_OUTPUT, "\a");
    } else if (term_gfx & TERM_GFX_PNG_KITTY) {
	uint8_t first = 1;
	while (size > 0) {
		/* Output chunk header. */
		fprintf(TEXT_RENDER_OUTPUT, "\033_G");
		if (first) {
			first = 0;
			fprintf(TEXT_RENDER_OUTPUT, "f=100,");
		}
		fprintf(TEXT_RENDER_OUTPUT, "m=%d;", size > 3072);

		/* Output up to 3072 bytes (4096 after base64 encoding) per chunk. */
		for (i = 0; i < 1024; i++) {
			size -= 3;
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
    gfx_last = time(NULL);
}


static void
cli_render_process(void *priv)
{
    char buf[TEXT_RENDER_BUF_SIZE], *p;
    uint8_t has_changed,
	    chr, attr, attr77,
	    sgr_started, sgr_blackout,
	    sgr_ul, sgr_int, sgr_reverse,
	    sgr_blink, sgr_bg, sgr_fg,
	    prev_sgr_ul, prev_sgr_int, prev_sgr_reverse,
	    prev_sgr_blink, prev_sgr_bg, prev_sgr_fg,
	    new_cx, new_cy;
    uint16_t chr_attr, *line_fb_row;
    int xc, x;

    while (1) {
	thread_wait_event(render_data.wake_render_thread, -1);
	thread_reset_event(render_data.wake_render_thread);

	/* Output any requested arbitrary text. */
	if (render_data.output) {
		fprintf(TEXT_RENDER_OUTPUT, render_data.output);
		free(render_data.output);
		render_data.output = NULL;
	}

	/* Don't render if the wanted row overflows the buffers.
	   This also works as a "don't render now" flag (y = -1). */
	p = NULL;
	new_cx = cursor_x;
	new_cy = cursor_y;
	if (render_data.y >= TEXT_RENDER_BUF_LINES)
		goto next;

	/* Copy framebuffer while determining whether or not
	   it has changed, as well as the cursor position. */
	line_fb_row = line_framebuffer[render_data.y];
	has_changed = 0;
	for (xc = x = 0; xc < render_data.xlimit; xc += render_data.xinc, x++) {
		/* Compare and copy 16-bit character+attribute value. */
		chr_attr = *((uint16_t *) &render_data.fb[(render_data.fb_base << 1) & render_data.fb_mask]);
		if (chr_attr != line_fb_row[x]) {
			has_changed = 1;
			line_fb_row[x] = chr_attr;
		}

		/* If the cursor is on this location, set it as the new cursor position. */
		if ((render_data.fb_base == render_data.ca) && render_data.con) {
			new_cx = x;
			new_cy = render_data.y;
		}

		render_data.fb_base += render_data.fb_step;
	}

	/* Don't render if the framebuffer hasn't changed. */
	if (!has_changed)
		goto next;

	/* Start with a fresh state. */
	p = buf;
	sgr_started = prev_sgr_blink = prev_sgr_bg = prev_sgr_fg = prev_sgr_ul = prev_sgr_int = prev_sgr_reverse = 0;
	sgr_blackout = -1;

	/* Render each character. */
	for (xc = x = 0; xc < render_data.xlimit; xc += render_data.xinc, x++) {
		if (render_data.do_render) {
			chr_attr = line_fb_row[x];
			chr = chr_attr & 0xff;
			attr = chr_attr >> 8;
		} else {
			chr = attr = 0;
		}

		if (render_data.color) {
			/* Set foreground color. */
			sgr_fg = ansi_palette[attr & 15];
			if ((x == 0) || (sgr_fg != prev_sgr_fg)) {
				APPEND_SGR();
				p += text_render_setcolor(p, sgr_fg, 0);
				prev_sgr_fg = sgr_fg;
			}

			/* If blinking is enabled, use the top bit for that instead of bright background. */
			if (render_data.do_blink) {
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
				p += sprintf(p, sgr_blink ? ((term_ctl & TERM_CTL_RAPIDBLINK) ? "6" : "5") : "25");
				prev_sgr_blink = sgr_blink;
			}
		} else {
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
				sgr_blink = (attr & 0x80) && render_data.do_blink;
				if (sgr_blink != prev_sgr_blink) {
					APPEND_SGR();
					p += sprintf(p, sgr_blink ? ((term_ctl & TERM_CTL_RAPIDBLINK) ? "6" : "5") : "25");
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
		}

		/* Finish any SGRs we may have started. */
		if (sgr_started) {
			sgr_started = 0;
			p += sprintf(p, "m");
		}

		/* Add character. */
		p += sprintf(p, cp437[chr]);
	}

	/* Output rendered line. */
	p = buf;
next:
	text_render_updateline(p, render_data.y, new_cx, new_cy);
	render_data.y = -1;

	thread_set_event(render_data.render_complete);
    }
}


void
text_render_mda(int xlimit,
		uint8_t *fb, uint16_t fb_base,
		uint8_t do_render, uint8_t do_blink,
		uint16_t ca, uint8_t con)
{
    CHECK_INIT();

    thread_wait_event(render_data.render_complete, -1);
    thread_reset_event(render_data.render_complete);

    render_data.color = 0;
    render_data.y = fb_base / xlimit;
    render_data.xlimit = xlimit;
    render_data.xinc = 1;
    render_data.fb = fb;
    render_data.fb_base = fb_base;
    render_data.fb_mask = 0xfff;
    render_data.fb_step = 1;
    render_data.do_render = do_render;
    render_data.do_blink = do_blink;
    render_data.ca = ca;
    render_data.con = con;

    thread_set_event(render_data.wake_render_thread);
}


void
text_render_cga(uint8_t y,
		int xlimit, int xinc,
		uint8_t *fb, uint32_t fb_base, uint32_t fb_mask, uint8_t fb_step,
		uint8_t do_render, uint8_t do_blink,
		uint32_t ca, uint8_t con)
{
    CHECK_INIT();

    thread_wait_event(render_data.render_complete, -1);
    thread_reset_event(render_data.render_complete);

    render_data.color = 1;
    render_data.y = y;
    render_data.xlimit = xlimit;
    render_data.xinc = xinc;
    render_data.fb = fb;
    render_data.fb_base = fb_base;
    render_data.fb_mask = fb_mask;
    render_data.fb_step = fb_step;
    render_data.do_render = do_render;
    render_data.do_blink = do_blink;
    render_data.ca = ca;
    render_data.con = con;

    thread_set_event(render_data.wake_render_thread);
}


void
cli_render_write(char *s)
{
    CHECK_INIT();

    thread_wait_event(render_data.render_complete, -1);
    thread_reset_event(render_data.render_complete);

    render_data.output = s;

    thread_set_event(render_data.wake_render_thread);
}


void
text_render_menu(uint8_t y)
{
    /* Move to the specified line. */
    fprintf(TEXT_RENDER_OUTPUT, "\033[%d;1H", y + 1);

    /* Render line. */
    int entry_count = sizeof(menu_entries) / sizeof(menu_entries[0]), i;
    if (y == 0) {
	i = 30 - fprintf(TEXT_RENDER_OUTPUT, "\033[30;47m%s%s[ " EMU_NAME " CLI Menu ]", cp437[0xd5], cp437[0xcd]);
	for (i = 0; i < 28; i++)
		fprintf(TEXT_RENDER_OUTPUT, cp437[0xc4]);
    } else if (y <= entry_count) {
	fprintf(TEXT_RENDER_OUTPUT, "%s ", cp437[0xb3]);
	for (i = fprintf(TEXT_RENDER_OUTPUT, menu_entries[y - 1]);
	     i < menu_max_width; i++)
		fprintf(TEXT_RENDER_OUTPUT, " ");
	fprintf(TEXT_RENDER_OUTPUT, " %s", cp437[0xb3]);
    } else if (y == (entry_count + 1)) {
	fprintf(TEXT_RENDER_OUTPUT, cp437[0xc0]);
	for (i = 0; i < 28; i++)
		fprintf(TEXT_RENDER_OUTPUT, cp437[0xc4]);
	fprintf(TEXT_RENDER_OUTPUT, cp437[0xd9]);
    }
    fflush(TEXT_RENDER_OUTPUT);
}
