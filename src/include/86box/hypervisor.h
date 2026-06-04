/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Definitions for hypervisor CPU back-ends.
 *
 *
 *
 * Authors: RichardG, <richardg867@gmail.com>
 *
 *          Copyright 2026 RichardG.
 */
#ifndef EMU_HYPERVISOR_H
#define EMU_HYPERVISOR_H

#ifdef USE_HYPERVISOR
extern void hv_get_regs(void);
extern void hv_apply_regs(void);
extern void hv_recalc_mappings(uint32_t base, uint32_t size);
extern void hv_init(void);
extern void hv_close(void);
extern void hv_exec(int32_t cycs);
#endif

#endif
