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
/* Set while the guest is executed by the hypervisor instead of the CPU
   emulation. The CPU state in cpu_state is then not current: only the
   hypervisor holds it. */
extern int hv_active;
/* fpu_softfloat set by the hypervisor, not the configuration. */
extern int hv_fpu_softfloat_forced;
/* Set while the CPU emulation runs an RSM on the hypervisor's processor
   state (a hypervisor without SMM of its own, whose SMM is 86Box's). */
extern int hv_rsm;

extern int  hv_available(void);
extern void hv_exec(int32_t cycs);
extern void hv_close(void);

/* The CPU's view of the memory map changed in this range: the memory
   regions the hypervisor maps directly have to follow. */
extern void hv_mem_changed(uint64_t base, uint64_t size, uint32_t base_ignore);
/* The A20 gate changed. */
extern void hv_a20_changed(void);
/* The CPU was reset (reset_common()). */
extern void hv_cpu_reset(int hard);
/* Copy the guest's registers into cpu_state, for the debugging tools. */
extern void hv_sync_regs(void);
#else
#    define hv_active 0
#endif

#endif
