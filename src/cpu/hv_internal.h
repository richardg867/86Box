/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Interface between the host independent part of the hypervisor
 *          CPU back-ends (hv_common.c: time, the memory map, the processor
 *          state as the CPU emulation sees it, I/O and memory accesses)
 *          and the host's hypervisor API (kvm.c, whpx.c).
 *
 *
 *
 * Authors: RichardG, <richardg867@gmail.com>
 *
 *          Copyright 2026 RichardG.
 */
#ifndef EMU_HV_INTERNAL_H
#define EMU_HV_INTERNAL_H

/* How long the main loop's call runs the guest for, and the longest gap
   between calls that still counts as time passing (a pause does not). */
#define HV_QUANTUM_NS 1000000ULL
/* The guest's time is the host's while it runs. Between two stretches of it,
   while 86Box handles an exit, it advances by up to HV_EXIT_NS, about what an
   I/O cycle takes. Anything longer (a timer that blits a frame, a disk read,
   a memory slot change, the main loop, a pause) would be a jump in time the
   guest can see, mid-loop: a delay loop counting refresh toggles misses some,
   a PIT read skips counts (Linux's quick_pit_calibrate() gives up), a BIOS
   test of the timers fails. That time is held back as lag, and given back
   while the guest runs, at HV_CATCHUP_PCT of the time it runs for, or all at
   once while it is halted, in its timers' order. A lag past HV_MAX_LAG_NS is
   dropped: the guest falls behind, as the emulated CPU does when it cannot
   keep up. The guest's TSC is offset by the lag, so that it stays the clock
   the timers keep. */
#define HV_EXIT_NS     5000ULL
#define HV_CATCHUP_PCT 10ULL
#define HV_MAX_LAG_NS  200000000ULL
/* The least the guest runs for before a timer stops it. Each stop costs a
   few microseconds, and device timers can come every few (sound, the
   display's scanlines): those run late by up to this, in their order. An
   access to a device runs every timer due first, so that it sees no
   difference. */
#define HV_MIN_RUN_NS 50000ULL /* precise timers excepted, see TIMER_PRECISE */

#define HV_GRANULES  (1 << (32 - MEM_GRANULARITY_BITS))
#define HV_MAX_SLOTS 1024

/* The high memory area, which follows A20. */
#define HV_HMA      0x100000
#define HV_HMA_SIZE 0x10000

/* A run of granules mapped straight into the guest: a KVM memory slot, a
   WHPX GPA range. */
typedef struct hv_slot_t {
    uint32_t       start; /* granules, end exclusive */
    uint32_t       end;
    uint8_t       *host;
    uint8_t        ro;
    uint16_t       id;
    mem_mapping_t *dirty;     /* the mapping whose writes are logged */
    uint32_t       dirty_off; /* where the slot starts in it */
} hv_slot_t;

/* The virtual processor's registers that the CPU emulation works on, in
   the form KVM has them in. */
typedef struct hv_seg_t {
    uint64_t base;
    uint32_t limit; /* in bytes, the granularity applied */
    uint16_t selector;
    uint8_t  type;
    uint8_t  present;
    uint8_t  dpl;
    uint8_t  db;
    uint8_t  s;
    uint8_t  l;
    uint8_t  g;
    uint8_t  avl;
    uint8_t  unusable;
} hv_seg_t;

typedef struct hv_regs_t {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rsp;
    uint64_t rbp;
    uint64_t rip;
    uint64_t rflags;
} hv_regs_t;

typedef struct hv_dtable_t {
    uint64_t base;
    uint16_t limit;
} hv_dtable_t;

typedef struct hv_sregs_t {
    hv_seg_t    cs;
    hv_seg_t    ds;
    hv_seg_t    es;
    hv_seg_t    fs;
    hv_seg_t    gs;
    hv_seg_t    ss;
    hv_seg_t    tr;
    hv_seg_t    ldt;
    hv_dtable_t gdt;
    hv_dtable_t idt;
    uint64_t    cr0;
    uint64_t    cr2;
    uint64_t    cr3;
    uint64_t    cr4;
} hv_sregs_t;

typedef struct hv_fpu_t {
    uint8_t  fpr[8][16]; /* ST(0) to ST(7), as FXSAVE has them */
    uint16_t fcw;
    uint16_t fsw;
    uint8_t  ftwx; /* abridged, as FXSAVE has it */
    uint16_t last_opcode;
    uint64_t last_ip;
    uint64_t last_dp;
} hv_fpu_t;

/* The virtual processor's state, loaded into the CPU emulation's to work on
   (hv_state_begin()), and what changed of it put back (hv_state_end()). */
typedef struct hv_state_t {
    hv_regs_t  regs;
    hv_sregs_t sregs;
    hv_fpu_t   fpu;
    x86seg     old[8];
    uint32_t   old_cr[4];
    uint32_t   old_dt[4];
} hv_state_t;

/* hv_common.c */
extern int      hv_as_count; /* address spaces: 2 with SMM's own */
extern hv_slot_t hv_slots[2][HV_MAX_SLOTS];
extern int      hv_nslots[2];
extern uint8_t  hv_ro_flips[2][0x100000 >> 14];
#define HV_PIN_FLIPS 4    /* flips before it is kept read only */
#define HV_PIN_NEVER 0xff /* the hypervisor could not deliver an event into it */
extern int      hv_pin_ok;
extern int      hv_npending;
extern int      hv_pending_all;
extern int      hv_in_recalc;

extern uint64_t hv_ns_last;    /* host clock at the last accounting */
extern uint64_t hv_lag_ns;     /* host time held back from the guest */
extern uint64_t hv_behind_ns;  /* the guest's time behind the host's since the start */
extern uint64_t hv_tsc_frac;
extern uint64_t hv_tsc_per_ns; /* 32.32 */
extern double   hv_cpuclock;

extern int hv_halted; /* 1 HLT, 2 an x87 instruction waiting for FERR#'s interrupt */
extern int hv_reset_pending;
extern int hv_smi_halted; /* the SMI ended a HLT */

extern uint64_t hv_stat_emulated;
extern uint64_t hv_stat_emulated_next;

extern double      cpuclock;
extern pc_timer_t *timer_head;

extern void     hv_rate_update(void);
extern void     hv_advance(uint64_t ns);
extern void     hv_account_run(int halted);
extern void     hv_account_out(void);
extern uint64_t hv_ts_to_host(uint64_t ts);
extern uint64_t hv_timer_deadline(void);
extern uint64_t hv_stop_deadline(void);

static inline void
hv_timers(void)
{
    if (timer_head && TIMER_VAL_LESS_THAN_VAL(timer_target, (uint64_t) tsc))
        timer_process();
}

extern uint8_t *hv_slot_at(int as, uint32_t g, int *ro, mem_mapping_t **dirty);
extern void     hv_dirty_slot(int as, hv_slot_t *slot);
extern void     hv_dirty_all(void);
extern void     hv_mem_init(void);
extern void     hv_mem_flush(void);

extern void hv_load_state(const hv_regs_t *regs, const hv_sregs_t *sregs);
extern void hv_state_begin(hv_state_t *st);
extern void hv_state_end(hv_state_t *st);
extern void hv_emulate(void);
extern int  hv_lin_readl(const hv_sregs_t *sr, uint32_t lin, uint32_t *val);
extern void hv_pin_poll(uint64_t cr0);
extern int  hv_pin_failed(uint64_t gpa);
extern void hv_dump(const char *why);
extern void hv_smm_bus(int smm);
extern int  hv_msr_access(uint32_t index, int write, uint64_t *data);
extern void hv_io_access(uint16_t port, int size, int out, uint8_t *data, uint32_t count);
extern void hv_mmio_access(uint32_t addr, uint8_t *data, uint32_t len, int write);
extern int  hv_halt(uint64_t end, int if_flag);

static inline int
hv_nmi_pending(void)
{
    /* The processor's NMI blocking is the hypervisor's; only the chipset's
       mask is 86Box's. */
    return nmi && (nmi_mask || (nmi & 2));
}

/* The host's API layer (kvm.c, whpx.c) */
extern const char *hv_name; /* in messages */

extern uint64_t hv_clock_ns(void);
/* The virtual machine is there, for the memory map to be followed. */
extern int      hv_be_ready(void);
/* Host memory that shows the first 64 KB of RAM or the high memory area,
   whichever A20 makes the latter, without slot changes: NULL if there is
   none, and slots follow A20 instead. */
extern uint8_t *hv_be_hma_window(void);
/* A20 changed: the window, if any, has to follow. */
extern void     hv_be_hma_follow(void);
extern void     hv_be_slot_map(int as, const hv_slot_t *slot);
extern void     hv_be_slot_unmap(int as, const hv_slot_t *slot);
/* The pages the guest wrote in a slot since the last call, a bit each, the
   log cleared. */
extern void     hv_be_dirty_log(int as, const hv_slot_t *slot, uint64_t *bitmap);
/* The memory map in granules [lo, hi) of address space 0 changed: queued
   writes (MEM_MAPPING_DEFER_WRITES) follow, if the host has them. */
extern void     hv_be_zones(uint32_t lo, uint32_t hi);
/* The virtual processor's registers; a NULL part is left alone. Getting
   returns -1 if it failed, errno set on a POSIX host; setting does not
   fail, or is fatal. */
extern int      hv_be_get_state(hv_regs_t *regs, hv_sregs_t *sregs, hv_fpu_t *fpu);
extern void     hv_be_set_state(const hv_regs_t *regs, const hv_sregs_t *sregs, const hv_fpu_t *fpu);
/* Halted: wait for the host's clock to reach deadline, or less. */
extern void     hv_be_sleep(uint64_t deadline);

#endif
