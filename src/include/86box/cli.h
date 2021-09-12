/*
 * 86Box	A hypervisor and IBM PC system emulator that specializes in
 *		running old operating systems and software designed for IBM
 *		PC systems and compatibles from 1981 through fairly recent
 *		system designs based on the PCI bus.
 *
 *		This file is part of the 86Box distribution.
 *
 *		Definitions for the command line interface.
 *
 *
 *
 * Author:	RichardG, <richardg867@gmail.com>
 *
 *		Copyright 2021 RichardG.
 */
#ifndef EMU_CLI_H
# define EMU_CLI_H

#define CLI_RENDER_OUTPUT	stderr

#ifdef USE_CLI
#define CLI_RENDER_MAX_LINES	60
#define CLI_RENDER_FB_SIZE	150
#define CLI_RENDER_ANSIBUF_SIZE	4096	/* good for a fully packed SVGA 150-column line with some margin */
#define CLI_RENDER_GFXBUF_W	2048
#define CLI_RENDER_GFXBUF_H	2048


enum {
    /* No color capability. */
    TERM_COLOR_NONE = 0x00,
    /* 8 ANSI colors. */
    TERM_COLOR_3BIT	= 0x01,
    /* 8 ANSI colors in dark and bright variants. */
    TERM_COLOR_4BIT	= TERM_COLOR_3BIT | 0x02,
    /* xterm 256-color palette. */
    TERM_COLOR_8BIT	= TERM_COLOR_4BIT | 0x04,
    /* True color with arbitrary RGB values. */
    TERM_COLOR_24BIT	= TERM_COLOR_8BIT | 0x08
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
    /* DEC Sixel graphics. */
    TERM_GFX_SIXEL	= 0x01,
    /* PNG image rendering through the iTerm2 protocol. */
    TERM_GFX_PNG	= 0x02,
    /* PNG image rendering through the kitty protocol. */
    TERM_GFX_PNG_KITTY	= 0x04
};

enum {
    RENDER_SIDEBAND_CPR = 0,
    RENDER_SIDEBAND_DECRQSS_COLOR,
    RENDER_SIDEBAND_MAX
};

typedef struct {
    uint8_t	color_level, ctl_level, gfx_level,
		can_input, cpr, decrqss_color,
		size_x, size_y;

    int		(*setcolor)(char *p, uint8_t index, uint8_t is_background);
} cli_term_t;


/* cli.c */
extern cli_term_t cli_term;

/* cli_render.c */
extern const uint8_t cga_ansi_palette[];

/* video.c */
extern volatile int cli_blit;


/* cli.c */
extern void	cli_term_setcolor(uint8_t level);
extern void	cli_term_setsize(int size_x, int size_y, char *source);

extern void	cli_init();
extern void	cli_close();

/* cli_input.c */
extern void	cli_input_init();
extern void	cli_input_close();

/* cli_render.c */
extern void	cli_render_blank();
extern void	cli_render_gfx(char *str);
extern void	cli_render_gfx_box(char *str);
extern void	cli_render_gfx_blit(uint32_t *buf, int w, int h);
extern void	cli_render_cga(uint8_t cy, uint8_t rowcount,
			       int xlimit, int xinc,
			       uint8_t *fb, uint32_t fb_base, uint32_t fb_mask, uint8_t fb_step,
			       uint8_t do_render, uint8_t do_blink,
			       uint32_t ca, uint8_t con);
extern void	cli_render_mda(int xlimit, uint8_t rowcount,
			       uint8_t *fb, uint16_t fb_base,
			       uint8_t do_render, uint8_t do_blink,
			       uint16_t ca, uint8_t con);

extern void	cli_render_write(int slot, char *s);
extern void	cli_render_write_title(wchar_t *s);

extern void	cli_render_monitorenter();
extern void	cli_render_monitorexit();

extern int	cli_render_setcolor_none(char *p, uint8_t index, uint8_t is_background);
extern void	cli_render_setcolorlevel();
extern void	cli_render_setpal(uint8_t index, uint32_t color);
extern void	cli_render_updatescreen();

extern void	cli_render_init();
extern void	cli_render_close();
#endif

/* cli_monitor.c */
extern void	cli_monitor_thread(void *priv);
extern void	cli_monitor_init(uint8_t independent);
extern void	cli_monitor_close();

#endif
