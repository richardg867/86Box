/*
 * 86Box	A hypervisor and IBM PC system emulator that specializes in
 *		running old operating systems and software designed for IBM
 *		PC systems and compatibles from 1981 through fairly recent
 *		system designs based on the PCI bus.
 *
 *		This file is part of the 86Box distribution.
 *
 *		Renderers for ANSI text output.
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
#ifdef _WIN32
# include <windows.h>
# include <VersionHelpers.h>
#endif
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/mem.h>
#include <86box/timer.h>
#include <86box/video.h>
#include <86box/vid_mda.h>
#include <86box/vid_svga.h>
#include <86box/vid_text_render.h>


#define TEXT_RENDER_OUTPUT	stdout
#define TEXT_RENDER_BUFSIZE	4096

#define APPEND_SGR()	if (!sgr_started) { \
				sgr_started = 1; \
				p += sprintf(p, "\033["); \
				if (x == 0) \
					p += sprintf(p, "0;"); \
			} else { \
				p += sprintf(p, ";"); \
			}


enum {
    TERM_MONO = 0,
    TERM_8COLOR = 8,
    TERM_16COLOR = 16,
    TERM_256COLOR = 254,
    TERM_TRUECOLOR = 255
};


/* Lookup table for converting CGA colors to the ANSI palette. */
const uint8_t ansi_palette[] = {
     0,  4,  2,  6,  1,  5,  3,  7, /* regular */
     8, 12, 10, 14,  9, 13, 11, 15  /* bright */
};

/* Lookup table for converting code page 437 to UTF-8. */
// s = ''; i = 0; for (var x of $('#thetable').find('td')) { c = $(x).find('small')[0].innerText.split('\n')[1]; s += 'L"\\u' + c + '", '; if (++i % 16 == 0) s += '\n'; }
// s = ''; i = 0; for (var x of $('#thetable').find('td')) { c = $(x).find('small')[0].innerText.split('\n')[1]; s += ('"' + encodeURI(String.fromCodePoint(parseInt(c, 16))).replace(/%/g, '\\x') + '",').padEnd(16); if (++i % 16 == 0) s += '\n'; }
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

/* Lookup table for terminal types. */
static const struct {
    const char	  *name;
    const uint8_t type;
} term_types[] = {
    {"iterm",		TERM_TRUECOLOR},
    {"iterm2",		TERM_TRUECOLOR},
    {"kitty",		TERM_TRUECOLOR},
    {"konsole",		TERM_TRUECOLOR},
    {"linux",		TERM_TRUECOLOR},
    {"mintty",		TERM_TRUECOLOR},
    {"termite",		TERM_TRUECOLOR},
    {"tmux",		TERM_TRUECOLOR},
    {"vte",		TERM_TRUECOLOR},
    {"xfce",		TERM_TRUECOLOR},
    {"xterm-24bit",	TERM_TRUECOLOR},
    {"xterm-24bits",	TERM_TRUECOLOR},
    {"xterm-256color",	TERM_TRUECOLOR},
    {"putty",		TERM_16COLOR},
    {"xterm-16color",	TERM_16COLOR},
    {"vt100",		TERM_MONO},
    {"vt220",		TERM_MONO},
    {NULL, 0} /* assume TERM_8COLOR for other terminals */
};


uint8_t text_render_initialized = 0;
uint8_t text_render_term = TERM_8COLOR;
uint8_t text_render_cx, text_render_cy;
uint32_t text_render_palette[16];
uint32_t *text_render_256col;
char text_render_line_buffer[50][TEXT_RENDER_BUFSIZE];
char text_render_gfx_type[16] = "\0";
int text_render_gfx_w = -1, text_render_gfx_h = -1;


/* Callbacks specific to each color capability level. */
int (*text_render_setcolor)(char *p, uint8_t idx, uint8_t bg);
void (*text_render_setpal)(uint8_t index, uint32_t color) = text_render_setpal_init;


int
text_render_setcolor_8(char *p, uint8_t idx, uint8_t bg)
{
    /* Set a color from the ANSI 8-color palette, ignoring the bright bit. */
    return sprintf(p, "%d", (bg ? 40 : 30) + (idx & 7));
}

int
text_render_setcolor_16(char *p, uint8_t idx, uint8_t bg) {
    /* Set a color from the ANSI 16-color palette. */
    uint8_t pre_attr = 0;
    uint8_t sgr = (bg ? 40 : 30) + (idx & 7);

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
text_render_setcolor_256(char *p, uint8_t idx, uint8_t bg)
{
    /* Set a color from the 256-color palette using the approximations calculated by text_render_setpal_256 */
    uint8_t approx = text_render_palette[idx];
    if (approx < 8) /* save bandwidth by using standard SGRs on 8-color palette colors */
    	return sprintf(p, "%d", (bg ? 40 : 30) + approx);
    else
    	return sprintf(p, "%d;5;%d", bg ? 48 : 38, approx);
}

int
text_render_setcolor_true(char *p, uint8_t idx, uint8_t bg)
{
    /* Set a full RGB color. */
    uint32_t color = text_render_palette[idx];
    return sprintf(p, "%d;2;%d;%d;%d", bg ? 48 : 38, (color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff);
}

void
text_render_setpal_noop(uint8_t index, uint32_t color)
{
    /* No palette mapping on low-color modes. */
}

void
text_render_setpal_256(uint8_t index, uint32_t color)
{
    /* Look through the 256-color palette for the closest color to the desired one. */
    uint32_t palette_color;
    uint8_t best_idx = 0, rdif, gdif, bdif;
    double candidate, best = INFINITY;

    for (int i = 0; i < 256; i++) {
	palette_color = text_render_256col[i];
	rdif = ((palette_color >> 16) & 0xff) - ((color >> 16) & 0xff);
	gdif = ((palette_color >> 8) & 0xff) - ((color >> 8) & 0xff);
	bdif = (palette_color & 0xff) - (color & 0xff);

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
text_render_setpal_true(uint8_t index, uint32_t color)
{
    /* True color terminals can be given the full RGB color. */
    text_render_palette[index] = color;
}

void
text_render_setpal_init(uint8_t index, uint32_t color)
{
    if (!text_render_initialized)
    	text_render_init();

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

void
text_render_fillcolortable(uint32_t *table, uint16_t count)
{
    /* Fill a color table with up to a 256-color palette.
       Algorithm from Linux's vt.c */
    uint32_t color;
    for (uint16_t i = 0; i < count; i++) {
	if (i < 8) { /* ANSI 8-color */
		color = 0;
		if (i & 1)
			color |= 0xaa0000;
		if (i & 2)
			color |= 0x00aa00;
		if (i & 4)
			color |= 0x0000aa;
	} else if (i < 16) { /* ANSI 16-color (bright) */
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
		color  = i * 10 - 2312;
		color |= color << 8;
		color |= (color & 0xff) << 16;
	}
	table[i] = color;
    }
}

void
text_render_init()
{
    int i;

    text_render_initialized = 1;

    /* Detect this terminal's color capability level. */
    char *colorterm = getenv("COLORTERM");
    if (colorterm && (strcasecmp(colorterm, "truecolor") || strcasecmp(colorterm, "24bit")))
    	text_render_term = TERM_TRUECOLOR;
    else {
    	/* Try the lookup table. */
    	char *term = getenv("TERM");
    	if (term) {
    		i = 0;
    		while (term_types[i].name) {
    			if (strcasecmp(term, term_types[i].name)) {
    				text_render_term = term_types[i].type;
    				break;
    			}
    		}
    	}
#ifdef _WIN32
    	else {
    		/* Assume a missing TERM on Windows means we're on cmd. */
    		if (IsWindows10OrGreater()) /* true color is supported on Windows 10 */
    			text_render_term = TERM_TRUECOLOR;
    		else /* older versions are limited to 16 colors */
    			text_render_term = TERM_16COLOR;
    	}
#endif
    }

    /* Initialize palette tables for high-color terminals. */
    if (text_render_term >= TERM_TRUECOLOR) {
    	/* Fill standard ANSI color values. */
    	text_render_fillcolortable(text_render_palette, sizeof(text_render_palette) / sizeof(text_render_palette[0]));
    } else if (text_render_term >= TERM_256COLOR) {
    	/* Map to ANSI colors at first. */
    	for (i = 0; i < sizeof(text_render_palette) / sizeof(text_render_palette[0]); i++)
    		text_render_palette[i] = i;

    	/* Fill color values. */
    	text_render_256col = malloc(256 * sizeof(uint32_t));
    	text_render_fillcolortable(text_render_256col, 256);
    }

    /* Set the correct setcolor function for this terminal's color capability. */
    switch (text_render_term) {
    	case TERM_8COLOR:
    		text_render_setcolor = text_render_setcolor_8;
    		text_render_setpal = text_render_setpal_noop;
    		break;

    	case TERM_16COLOR:
    		text_render_setcolor = text_render_setcolor_16;
    		text_render_setpal = text_render_setpal_noop;
    		break;

    	case TERM_256COLOR:
    		text_render_setcolor = text_render_setcolor_256;
    		text_render_setpal = text_render_setpal_256;
    		break;

    	case TERM_TRUECOLOR:
    		text_render_setcolor = text_render_setcolor_true;
    		text_render_setpal = text_render_setpal_true;
    		break;
    }

    /* Start with the cursor disabled. */
    text_render_cx = text_render_cy = -1;
    text_render_updatecursor();
}


void
text_render_blank()
{
    if (!text_render_initialized)
    	text_render_init();

    /* Clear screen. */
    fprintf(TEXT_RENDER_OUTPUT, "\033[2J");

    /* Disable cursor and flush. */
    text_render_cx = text_render_cy = -1;
    text_render_updatecursor();
}

void
text_render_gfx(char *type)
{
    uint8_t render = 0, i;
    char buf[256], boxattr[] = "\033[30;47m", resetattr[] = "\033[0m";

    int w = get_actual_size_x();
    if (w != text_render_gfx_w) {
    	render = 1;
    	text_render_gfx_w = w;
    }

    int h = get_actual_size_y();
    if (h != text_render_gfx_h) {
    	render = 1;
    	text_render_gfx_h = h;
    }

    if (!strncmp(type, text_render_gfx_type, sizeof(text_render_gfx_type) - 1)) {
    	render = 1;
    	strncpy(text_render_gfx_type, type, sizeof(text_render_gfx_type) - 1);
    }

    if (render) {
    	sprintf(buf, "%s %dx%d", type, get_actual_size_x(), get_actual_size_y());
    	fprintf(TEXT_RENDER_OUTPUT, "\033[2J\033[1;1H%s%s%s", resetattr, boxattr, cp437[0xc9]);
    	for (i = 0; i < strlen(buf); i++)
    		fprintf(TEXT_RENDER_OUTPUT, cp437[0xcd]);
    	fprintf(TEXT_RENDER_OUTPUT, "%s%s\033[2;1H%s%s%s%s%s\033[3;1H%s%s", cp437[0xbb], resetattr, boxattr, cp437[0xba], buf, cp437[0xba], resetattr, boxattr, cp437[0xc8]);
    	for (i = 0; i < strlen(buf); i++)
    		fprintf(TEXT_RENDER_OUTPUT, cp437[0xcd]);
    	fprintf(TEXT_RENDER_OUTPUT, "%s%s", cp437[0xbc], resetattr);

    	/* Disable cursor and flush. */
    	text_render_cx = text_render_cy = -1;
    	text_render_updatecursor();
    }
}

void
text_render_mda(mda_t *mda, uint16_t ca, uint8_t cy)
{
    char buf[TEXT_RENDER_BUFSIZE], *p;
    int x;
    uint8_t chr, attr, cx = 0, new_cx = text_render_cx, new_cy = text_render_cy;
    uint8_t sgr_started = 0, sgr_blackout = 1, sgr_ul, sgr_int, sgr_blink, sgr_invert;
    uint8_t prev_sgr_ul = 0, prev_sgr_int = 0, prev_sgr_blink = 0, prev_sgr_invert = 0;

    p = buf;
    p += sprintf(p, "\033[%d;1H\033[0m", cy + 1);

    for (x = 0; x < mda->crtc[1]; x++) {
    	chr  = mda->vram[(mda->ma << 1) & 0xfff];
        attr = mda->vram[((mda->ma << 1) + 1) & 0xfff];

        if ((attr == 0x00) || (attr == 0x08) || (attr == 0x80) || (attr == 0x88)) {
        	/* Create a blank space by discarding all attributes then printing a space. */
        	if (!sgr_blackout) {
        		APPEND_SGR();
        		p += sprintf(p, "0");
        		sgr_blackout = 1;
        		prev_sgr_ul = prev_sgr_int = prev_sgr_blink = prev_sgr_invert = 0;
        	}
        	chr = ' ';
        } else {
        	sgr_blackout = 0;

	        sgr_ul = (attr & 3) == 1;
	        if (sgr_ul != prev_sgr_ul) {
	        	APPEND_SGR();
	        	p += sprintf(p, sgr_ul ? "4" : "24");
	        	prev_sgr_ul = sgr_ul;
	        }

	        sgr_int = attr & 0x08;
	        if (sgr_int != prev_sgr_int) {
	        	APPEND_SGR();
	        	p += sprintf(p, sgr_int ? "1" : "22");
	        	prev_sgr_int = sgr_int;
	        }

	        sgr_blink = (mda->ctrl & 0x20) && (attr & 0x80);
	        if (sgr_blink != prev_sgr_blink) {
			APPEND_SGR();
			p += sprintf(p, sgr_blink ? "5" : "25");
			prev_sgr_blink = sgr_blink;
	        }

	        /* Each inverted attribute has its intricacies, but having
	           them all be the same is good enough for this purpose. */
	        sgr_invert = (attr == 0x70) || (attr == 0x78) || (attr == 0xf0) || (attr == 0xf8);
	        if (sgr_invert != prev_sgr_invert) {
	        	APPEND_SGR();
	        	p += sprintf(p, sgr_invert ? "7" : "27");
	        	prev_sgr_invert = sgr_invert;
	        }
	}

        if ((mda->ma == ca) && mda->con) {
		new_cx = cx;
		new_cy = cy;
	}

	if (sgr_started) {
		sgr_started = 0;
		p += sprintf(p, "m");
	}

        p += sprintf(p, cp437[chr]);

        mda->ma++;
        cx++;
    }

    if (memcmp(buf, text_render_line_buffer[cy], p - buf)) {
    	fprintf(TEXT_RENDER_OUTPUT, buf);
    	text_render_cx = ~new_cx; /* force the cursor to reposition, which also flushes the output */
    	memcpy(text_render_line_buffer[cy], buf, p - buf);
    }

    if ((new_cx != text_render_cx) || (new_cy != text_render_cy)) {
    	if (mda->con) {
    		text_render_cx = new_cx;
    		text_render_cy = new_cy;
    	} else {
    		text_render_cx = text_render_cy = -1;
    	}
    	text_render_updatecursor();
    }
}

void
text_render_svga(svga_t *svga, int xinc, uint8_t cy)
{
    char buf[TEXT_RENDER_BUFSIZE], *p;
    int x;
    uint8_t chr, attr, cx = 0, new_cx = text_render_cx, new_cy = text_render_cy;
    uint8_t sgr_started = 0, sgr_bg, sgr_fg, sgr_blink;
    uint8_t prev_sgr_bg = 0, prev_sgr_fg = 0, prev_sgr_blink = 0;

    p = buf;
    p += sprintf(p, "\033[%d;1H", cy + 1);

    for (x = 0; x < (svga->hdisp + svga->scrollcache); x += xinc) {
	if (svga->crtc[0x17] & 0x80) {
		chr  = svga->vram[(svga->ma << 1) & svga->vram_display_mask];
		attr = svga->vram[((svga->ma << 1) + 1) & svga->vram_display_mask];
	} else
		chr = attr = 0;

	sgr_fg = ansi_palette[attr & 15];
	if ((x == 0) || (sgr_fg != prev_sgr_fg)) {
		APPEND_SGR();
		p += text_render_setcolor(p, sgr_fg, 0);
		prev_sgr_fg = sgr_fg;
	}

	if (svga->attrregs[0x10] & 0x08) {
		sgr_blink = attr & 0x80;
		attr &= 0x7f;
	} else
		sgr_blink = 0;

	sgr_bg = ansi_palette[attr >> 4];
	if ((x == 0) || (sgr_bg != prev_sgr_bg)) {
		APPEND_SGR();
		p += text_render_setcolor(p, sgr_bg, 1);
		prev_sgr_bg = sgr_bg;
	}

	if ((x == 0) || (sgr_blink != prev_sgr_blink)) {
		APPEND_SGR();
		p += sprintf(p, sgr_blink ? "5" : "25");
		prev_sgr_blink = sgr_blink;
	}

	if ((svga->ma == svga->ca) && svga->con) {
		new_cx = cx;
		new_cy = cy;
	}

	if (sgr_started) {
		sgr_started = 0;
		p += sprintf(p, "m");
	}

	p += sprintf(p, cp437[chr]);

	svga->ma += 4;
	cx++;
    }

    if (memcmp(buf, text_render_line_buffer[cy], p - buf)) {
    	fprintf(TEXT_RENDER_OUTPUT, buf);
    	text_render_cx = ~new_cx; /* force the cursor to reposition, which also flushes the output */
    	memcpy(text_render_line_buffer[cy], buf, p - buf);
    }

    if ((new_cx != text_render_cx) || (new_cy != text_render_cy)) {
    	if (svga->con) {
    		text_render_cx = new_cx;
    		text_render_cy = new_cy;
    	} else {
    		text_render_cx = text_render_cy = -1;
    	}
    	text_render_updatecursor();
    }
}

void
text_render_svga_gfx(svga_t *svga)
{
    
}