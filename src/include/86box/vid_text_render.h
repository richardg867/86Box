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

extern const uint8_t	ansi_palette[];

void	text_render_init();
void	text_render_setpal_init(uint8_t index, uint32_t color);
void	text_render_blank();
void	text_render_gfx(char *type);

#ifdef TEXT_RENDER_MDA
void	text_render_mda(mda_t *mda, uint16_t ca, uint8_t cy);
#endif
#ifdef TEXT_RENDER_SVGA
void	text_render_svga(svga_t *svga, int xinc, uint8_t cy);
#endif

extern void	(*text_render_setpal)(uint8_t index, uint32_t color);
