/*
 * 86Box	A hypervisor and IBM PC system emulator that specializes in
 *		running old operating systems and software designed for IBM
 *		PC systems and compatibles from 1981 through fairly recent
 *		system designs based on the PCI bus.
 *
 *		This file is part of the 86Box distribution.
 *
 *		Definitions for ANSI text output renderers.
 *
 *
 *
 * Author:	RichardG, <richardg867@gmail.com>
 *
 *		Copyright 2021 RichardG.
 */
#ifndef EMU_VID_TEXT_RENDER_H
# define EMU_VID_TEXT_RENDER_H

#define TEXT_RENDER_OUTPUT	stderr


extern const uint8_t	ansi_palette[];

extern uint8_t	cli_initialized;
extern uint8_t	cli_menu;


void	text_render_init();
void	text_render_setpal_init(uint8_t index, uint32_t color);
void	text_render_blank();
void	text_render_gfx(char *str);
void	text_render_gfx_box(char *str);
void	text_render_gfx_image(char *fn);

void	text_render_mda(int xlimit,
			uint8_t *fb, uint16_t fb_base,
			uint8_t do_render, uint8_t do_blink,
			uint16_t ca, uint8_t con);
void	text_render_cga(uint8_t cy,
			int xlimit, int xinc,
			uint8_t *fb, uint32_t fb_base, uint32_t fb_mask, uint8_t fb_step,
			uint8_t do_render, uint8_t do_blink,
			uint32_t ca, uint8_t con);

void	cli_render_write(char *s);
void	text_render_menu(uint8_t cy);


extern void	(*text_render_setpal)(uint8_t index, uint32_t color);

#endif
