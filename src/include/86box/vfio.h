/*
 * 86Box	A hypervisor and IBM PC system emulator that specializes in
 *		running old operating systems and software designed for IBM
 *		PC systems and compatibles from 1981 through fairly recent
 *		system designs based on the PCI bus.
 *
 *		This file is part of the 86Box distribution.
 *
 *		Definitions for Virtual Function I/O PCI passthrough.
 *
 *
 *
 * Author:	RichardG, <richardg867@gmail.com>
 *
 *		Copyright 2021 RichardG.
 */
#ifndef EMU_VFIO_H
# define EMU_VFIO_H

#ifdef __linux__
extern void	vfio_init();
extern void	vfio_close();
#else
# define vfio_init()
# define vfio_close()
#endif

#endif
