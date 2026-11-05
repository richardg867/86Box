/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Linux KVM hypervisor CPU back-end.
 *
 *          Time: the TSC that 86Box's timers count in follows the host's
 *          monotonic clock, at the emulated CPU's clock rate. It is
 *          brought up to date whenever the guest stops, before the exit is
 *          handled, so every device sees the time at which the guest
 *          accessed it. A POSIX timer aimed at this thread stops the guest
 *          when the next 86Box timer is due.
 *
 *          Memory: the CPU's view of the memory map (_mem_exec[],
 *          read_mapping[], write_mapping[]) is translated into KVM memory
 *          slots, in address space 0 for normal operation and address
 *          space 1 for SMM. Memory a mapping reads straight from its exec
 *          memory is mapped, read only unless its writes go there too;
 *          everything else stays an MMIO exit handled by 86Box.
 *
 *          SMM is KVM's: SMIs are injected with KVM_SMI, and KVM saves and
 *          restores the state and runs RSM.
 *
 *
 *
 * Authors: RichardG, <richardg867@gmail.com>
 *
 *          Copyright 2026 RichardG.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kvm.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include "x86.h"
#include "x86_interrupt.h"
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/nmi.h>
#include <86box/pic.h>
#include <86box/smram.h>
#include <86box/timer.h>
#include <86box/hypervisor.h>
#include <86box/plat_unused.h>
/* struct kvm_sregs has members of these names. */
#undef cs
#undef ds
#undef es
#undef ss
#undef gs
#undef cr0

#ifndef sigev_notify_thread_id
#    define sigev_notify_thread_id _sigev_un._tid
#endif

#ifdef ENABLE_KVM_LOG
int kvm_do_log = ENABLE_KVM_LOG;

static void
kvm_log(const char *fmt, ...)
{
    va_list ap;

    if (kvm_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define kvm_log(fmt, ...)
#endif

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

#define HV_GRANULES   (1 << (32 - MEM_GRANULARITY_BITS))
#define HV_MAX_SLOTS  1024

typedef struct hv_slot_t {
    uint32_t       start; /* granules, end exclusive */
    uint32_t       end;
    uint8_t       *host;
    uint8_t        ro;
    uint16_t       id;
    mem_mapping_t *dirty; /* the mapping whose writes are logged */
    uint32_t       dirty_off; /* where the slot starts in it */
} hv_slot_t;

int hv_active = 0;

static int             kvm_fd   = -1;
static int             vm_fd    = -1;
static int             cpu_fd   = -1;
static int             run_size = 0;
static struct kvm_run *run      = MAP_FAILED;
static int             hv_avail = -1;

static int hv_as_count; /* 2 with KVM's SMM */
static int hv_has_smm;

static hv_slot_t hv_slots[2][HV_MAX_SLOTS];
static int       hv_nslots[2];
static uint8_t   hv_slot_id_used[2][HV_MAX_SLOTS];
static hv_slot_t hv_runs[HV_MAX_SLOTS];

/* Coalesced MMIO: writes KVM queues in a ring without leaving the kernel,
   for mappings with MEM_MAPPING_DEFER_WRITES that are not mapped directly.
   86Box carries them out, in order, before it handles the next exit. */
#define HV_MAX_ZONES 64 /* KVM allows 100 */
typedef struct hv_zone_t {
    uint32_t start; /* granules, end exclusive */
    uint32_t end;
} hv_zone_t;
static struct kvm_coalesced_mmio_ring *hv_ring;
static uint32_t                        hv_ring_max;
static hv_zone_t                       hv_zones[HV_MAX_ZONES];
static int                             hv_nzones;
static uint64_t                        hv_stat_coalesced;
/* How often each 16 KB below 1 MB went between read only and writable. */
static uint8_t   hv_ro_flips[2][0x100000 >> 14];
static int       hv_in_recalc;

static int       hv_sig;
static sigset_t  hv_sigset;
static timer_t   hv_timer;
static int       hv_timer_valid;
static uint64_t  hv_armed_ns;
static pthread_t hv_thread;

static uint64_t hv_ns_last;    /* host clock at the last accounting */
static uint64_t hv_lag_ns;     /* host time held back from the guest */
static uint64_t hv_behind_ns;  /* the guest's time behind the host's since the start */
static uint64_t hv_tsc_frac;
static uint64_t hv_tsc_per_ns; /* 32.32 */
static double   hv_cpuclock;
static int      hv_tsc_attr;   /* KVM_VCPU_TSC_OFFSET is there */
static uint64_t hv_tsc_base;   /* the guest TSC's offset from the host's at the start */
static uint64_t hv_tsc_khz;
static int64_t  hv_tsc_applied; /* the lag the offset holds, in TSC ticks */

static int hv_halted;
static int hv_reset_pending;
static int hv_smi_entering;

/* Statistics, logged every few seconds with ENABLE_KVM_LOG. */
static uint64_t hv_stat_exits[64];
static uint64_t hv_stat_intr;
static uint64_t hv_stat_slot_ops;

extern double      cpuclock;
extern pc_timer_t *timer_head;

static void
hv_sig_handler(UNUSED(int sig))
{
    /* Only here to make KVM_RUN return. */
}

static inline uint64_t
hv_clock_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t) ts.tv_sec * 1000000000ULL) + ts.tv_nsec;
}

/* Time. */
static void
hv_rate_update(void)
{
    if (cpuclock != hv_cpuclock) {
        hv_cpuclock   = cpuclock;
        hv_tsc_per_ns = (uint64_t) (cpuclock / 1000000000.0 * 4294967296.0);
        if (!hv_tsc_per_ns)
            hv_tsc_per_ns = 1;
    }
}

/* The guest's clock moves on by ns. */
static void
hv_advance(uint64_t ns)
{
    uint128_t t = ((uint128_t) ns * hv_tsc_per_ns) + hv_tsc_frac;

    tsc        += (uint64_t) (t >> 32);
    hv_tsc_frac = (uint32_t) t;
}

/* Host time that passed while the guest ran or was halted: all of it, and
   some of the lag on top. */
static void
hv_account_run(int halted)
{
    uint64_t now = hv_clock_ns();
    uint64_t d   = now - hv_ns_last;
    uint64_t c;

    if ((int64_t) d <= 0)
        return;
    hv_ns_last = now;

    c = halted ? hv_lag_ns : ((d * HV_CATCHUP_PCT) / 100);
    if (c > hv_lag_ns)
        c = hv_lag_ns;
    hv_lag_ns -= c;
    hv_behind_ns -= c;
    hv_advance(d + c);
}

/* Host time that passed outside the guest, handling exits: up to HV_EXIT_NS
   of it, the rest held back. */
static void
hv_account_out(void)
{
    uint64_t now = hv_clock_ns();
    uint64_t d   = now - hv_ns_last;
    uint64_t a;

    if ((int64_t) d <= 0)
        return;
    hv_ns_last = now;

    a = (d > HV_EXIT_NS) ? HV_EXIT_NS : d;
    hv_advance(a);
    hv_lag_ns    += d - a;
    hv_behind_ns += d - a;
    if (hv_lag_ns > HV_MAX_LAG_NS) {
        kvm_log("KVM: dropping %" PRIu64 " ms of lag\n", (hv_lag_ns - HV_MAX_LAG_NS) / 1000000ULL);
        hv_lag_ns = HV_MAX_LAG_NS;
    }
}

/* Keep the guest's TSC on its clock: behind the host's by as much. */
static void
hv_tsc_sync(void)
{
    int64_t                ticks = (int64_t) (((uint128_t) hv_behind_ns * hv_tsc_khz) / 1000000ULL);
    uint64_t               off;
    struct kvm_device_attr attr = {
        .group = KVM_VCPU_TSC_CTRL,
        .attr  = KVM_VCPU_TSC_OFFSET,
        .addr  = (uintptr_t) &off
    };

    /* A microsecond of slack, not to set it at every exit. */
    if (!hv_tsc_attr || (llabs(ticks - hv_tsc_applied) < (int64_t) (hv_tsc_khz / 1000)))
        return;

    /* The guest may have written its TSC since: that moved the base. */
    if (ioctl(cpu_fd, KVM_GET_DEVICE_ATTR, &attr) == 0)
        hv_tsc_base = off + (uint64_t) hv_tsc_applied;

    off = hv_tsc_base - (uint64_t) ticks;
    if (ioctl(cpu_fd, KVM_SET_DEVICE_ATTR, &attr) < 0)
        fatal("KVM: setting the TSC offset failed: %s\n", strerror(errno));
    hv_tsc_applied = ticks;
}

/* A timer's due time on the host's clock. */
static inline uint64_t
hv_ts_to_host(uint64_t ts)
{
    int64_t rem = (int64_t) (ts - tsc);

    if (rem <= 0)
        return hv_ns_last;

    return hv_ns_last + (uint64_t) (((uint128_t) rem << 32) / hv_tsc_per_ns) + 1;
}

/* When the next timer is due, on the host's clock. */
static uint64_t
hv_timer_deadline(void)
{
    if (!timer_head)
        return hv_ns_last + 1000000000ULL;

    return hv_ts_to_host(timer_target);
}

/* When the guest is to be stopped for its timers: the next one, but no
   sooner than HV_MIN_RUN_NS from now unless it is a precise one (see
   TIMER_PRECISE), which is never run late. */
static uint64_t
hv_stop_deadline(void)
{
    uint64_t         deadline = hv_timer_deadline();
    const pc_timer_t *t;

    if ((int64_t) (deadline - (hv_ns_last + HV_MIN_RUN_NS)) >= 0)
        return deadline;

    for (t = timer_head; t != NULL; t = t->next) {
        if (t->flags & TIMER_PRECISE)
            return MIN(hv_ts_to_host(t->ts_integer), hv_ns_last + HV_MIN_RUN_NS);
        if ((int64_t) (hv_ts_to_host(t->ts_integer) - (hv_ns_last + HV_MIN_RUN_NS)) >= 0)
            break;
    }

    return hv_ns_last + HV_MIN_RUN_NS;
}

static inline void
hv_timers(void)
{
    if (timer_head && TIMER_VAL_LESS_THAN_VAL(timer_target, (uint64_t) tsc))
        timer_process();
}

/* Have the signal stop the guest at deadline. A timer that is already set
   to go off earlier is left alone: going off early costs an exit, nothing
   else. */
static void
hv_arm(uint64_t deadline)
{
    struct itimerspec its = { 0 };

    if (hv_armed_ns && ((int64_t) (hv_armed_ns - hv_ns_last) > 0) && ((int64_t) (hv_armed_ns - deadline) <= 0))
        return;

    its.it_value.tv_sec  = deadline / 1000000000ULL;
    its.it_value.tv_nsec = deadline % 1000000000ULL;
    if (timer_settime(hv_timer, TIMER_ABSTIME, &its, NULL) < 0)
        fatal("KVM: timer_settime failed: %s\n", strerror(errno));
    hv_armed_ns = deadline;
}

/* Swallow the signal, which stays pending, blocked, after it stopped
   KVM_RUN. */
static void
hv_drain_signal(void)
{
    struct timespec ts = { 0 };

    while (sigtimedwait(&hv_sigset, NULL, &ts) > 0)
        ;
}

/* Memory slots. */
static uint8_t *
hv_resolve(int as, uint32_t g, int *ro, mem_mapping_t **dirty)
{
    int      a20 = mem_a20_state || (as && unmask_a20_in_smm);
    int      writable;
    uint8_t *ret;

    /* With A20 masked, the high memory area is the first 64 KB again. Only
       that, which real mode code can reach, is followed. */
    if (!a20 && (g >= (0x100000 >> MEM_GRANULARITY_BITS)) && (g < (0x110000 >> MEM_GRANULARITY_BITS)))
        g -= (0x100000 >> MEM_GRANULARITY_BITS);

    ret = mem_granule_direct(g, &writable, dirty);
    *ro = !writable;
    return ret;
}

static void
hv_slot_ioctl(int as, int id, uint32_t start, uint32_t end, uint8_t *host, int ro, int log)
{
    struct kvm_userspace_memory_region r = {
        .slot            = (as << 16) | id,
        .flags           = (ro ? KVM_MEM_READONLY : 0) | (log ? KVM_MEM_LOG_DIRTY_PAGES : 0),
        .guest_phys_addr = (uint64_t) start << MEM_GRANULARITY_BITS,
        .memory_size     = (uint64_t) (end - start) << MEM_GRANULARITY_BITS,
        .userspace_addr  = (uintptr_t) host
    };

    hv_stat_slot_ops++;
    kvm_log("KVM: AS%d slot %d %08llX+%llX %s %p\n", as, id, r.guest_phys_addr, r.memory_size,
            ro ? "ro" : "rw", host);
    if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &r) < 0)
        fatal("KVM: AS%d slot %d at %08llX+%llX (host %p) failed: %s\n", as, id, r.guest_phys_addr,
              r.memory_size, host, strerror(errno));
}

/* Report the pages the guest wrote in a logged slot to their mapping. */
static void
hv_dirty_slot(int as, hv_slot_t *slot)
{
    static uint64_t     bitmap[(HV_GRANULES / 64) + 1];
    struct kvm_dirty_log log  = { 0 };
    uint32_t            pages = slot->end - slot->start;
    mem_mapping_t      *map   = slot->dirty;

    log.slot         = (as << 16) | slot->id;
    log.dirty_bitmap = bitmap;
    if (ioctl(vm_fd, KVM_GET_DIRTY_LOG, &log) < 0)
        fatal("KVM: GET_DIRTY_LOG failed: %s\n", strerror(errno));

    for (uint32_t w = 0; w < ((pages + 63) / 64); w++) {
        uint64_t bits = bitmap[w];

        while (bits) {
            int b = __builtin_ctzll(bits);

            bits &= bits - 1;
            map->direct_dirty(slot->host + ((size_t) (w * 64 + b) << MEM_GRANULARITY_BITS), map->direct_priv);
        }
    }
}

static void
hv_dirty_all(void)
{
    for (int as = 0; as < hv_as_count; as++) {
        for (int i = 0; i < hv_nslots[as]; i++) {
            if (hv_slots[as][i].dirty)
                hv_dirty_slot(as, &hv_slots[as][i]);
        }
    }
}

static uint8_t *hv_slot_at(int as, uint32_t g, int *ro, mem_mapping_t **dirty);

/* Carry out the writes KVM queued, in order. They are to address space 0's
   view, the only one with zones, which they were made in: this runs at every
   exit, before an SMM change is followed. */
static void
hv_drain(void)
{
    struct kvm_coalesced_mmio *ent;

    if (!hv_ring)
        return;

    while (hv_ring->first != hv_ring->last) {
        ent = &hv_ring->coalesced_mmio[hv_ring->first];
        switch (ent->len) {
            case 1:
                mem_view_write(MEM_VIEW_CPU_NORMAL, (uint32_t) ent->phys_addr, ent->data[0], 1);
                break;
            case 2:
                mem_view_write(MEM_VIEW_CPU_NORMAL, (uint32_t) ent->phys_addr, *(uint16_t *) ent->data, 2);
                break;
            case 4:
                mem_view_write(MEM_VIEW_CPU_NORMAL, (uint32_t) ent->phys_addr, *(uint32_t *) ent->data, 4);
                break;
            default:
                for (uint32_t i = 0; i < ent->len; i++)
                    mem_view_write(MEM_VIEW_CPU_NORMAL, (uint32_t) ent->phys_addr + i, ent->data[i], 1);
                break;
        }
        hv_stat_coalesced++;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        hv_ring->first = (hv_ring->first + 1) % hv_ring_max;
    }
}

static void
hv_zone_ioctl(int reg, uint32_t start, uint32_t end)
{
    struct kvm_coalesced_mmio_zone z = {
        .addr = (uint64_t) start << MEM_GRANULARITY_BITS,
        .size = (end - start) << MEM_GRANULARITY_BITS
    };

    kvm_log("KVM: %s coalesced zone %08llX+%X\n", reg ? "adding" : "removing", z.addr, z.size);
    if (ioctl(vm_fd, reg ? KVM_REGISTER_COALESCED_MMIO : KVM_UNREGISTER_COALESCED_MMIO, &z) < 0)
        pclog("KVM: coalesced zone %08llX+%X: %s\n", z.addr, z.size, strerror(errno));
}

/* The coalesced zones in granules [lo, hi) of address space 0, after its
   slots there were applied. */
static void
hv_apply_zones(uint32_t lo, uint32_t hi)
{
    hv_zone_t      runs[HV_MAX_ZONES];
    int            nr = 0;
    int            i;
    int            j;
    int            ro;
    mem_mapping_t *dirty;
    uint32_t       xlo = lo;
    uint32_t       xhi = hi;

    if (!hv_ring)
        return;

    /* Zones that overlap the range go, and come back for what is outside. */
    for (i = 0; i < hv_nzones; i++) {
        if ((hv_zones[i].end > lo) && (hv_zones[i].start < hi)) {
            xlo = MIN(xlo, hv_zones[i].start);
            xhi = MAX(xhi, hv_zones[i].end);
        }
    }

    for (uint32_t g = xlo; g < xhi; g++) {
        int want;

        if ((g >= lo) && (g < hi)) {
            want = (hv_slot_at(0, g, &ro, &dirty) == NULL) && write_mapping[g] &&
                   (write_mapping[g]->flags & MEM_MAPPING_DEFER_WRITES);
        } else {
            want = 0;
            for (i = 0; i < hv_nzones; i++)
                want |= (g >= hv_zones[i].start) && (g < hv_zones[i].end);
        }
        if (!want)
            continue;
        if (nr && (runs[nr - 1].end == g))
            runs[nr - 1].end++;
        else if (nr < HV_MAX_ZONES) {
            runs[nr].start = g;
            runs[nr].end   = g + 1;
            nr++;
        }
    }

    /* Unchanged? */
    for (i = 0, j = 0; i < hv_nzones; i++) {
        if ((hv_zones[i].end > xlo) && (hv_zones[i].start < xhi)) {
            if ((j >= nr) || (hv_zones[i].start != runs[j].start) || (hv_zones[i].end != runs[j].end))
                break;
            j++;
        }
    }
    if ((i == hv_nzones) && (j == nr))
        return;

    for (i = 0; i < hv_nzones;) {
        if ((hv_zones[i].end > xlo) && (hv_zones[i].start < xhi)) {
            hv_zone_ioctl(0, hv_zones[i].start, hv_zones[i].end);
            hv_zones[i] = hv_zones[--hv_nzones];
        } else
            i++;
    }
    for (i = 0; (i < nr) && (hv_nzones < HV_MAX_ZONES); i++) {
        hv_zone_ioctl(1, runs[i].start, runs[i].end);
        hv_zones[hv_nzones++] = runs[i];
    }
}

/* Slots never run across these: below 1 MB the chipset maps 16 KB at a time
   (shadow RAM, SMRAM), and the high memory area follows A20. Changing one of
   them then leaves the slots around it alone, the big one of RAM above it
   above all, which is costly to rebuild. */
static inline int
hv_slot_boundary(uint32_t g)
{
    if (g < (0x100000 >> MEM_GRANULARITY_BITS))
        return !(g & ((0x4000 >> MEM_GRANULARITY_BITS) - 1));

    return (g == (0x100000 >> MEM_GRANULARITY_BITS)) || (g == (0x110000 >> MEM_GRANULARITY_BITS));
}

/* What the slots of an address space map at a granule now. */
static uint8_t *
hv_slot_at(int as, uint32_t g, int *ro, mem_mapping_t **dirty)
{
    for (int i = 0; i < hv_nslots[as]; i++) {
        const hv_slot_t *slot = &hv_slots[as][i];

        if ((g >= slot->start) && (g < slot->end)) {
            *ro    = slot->ro;
            *dirty = slot->dirty;
            return slot->host + ((size_t) (g - slot->start) << MEM_GRANULARITY_BITS);
        }
    }

    return NULL;
}

/* Make the slots of an address space match the current view of the memory
   map in granules [lo, hi), which the CPU's view arrays hold for that
   address space's SMM state. Only slots that change are touched: a slot
   change makes KVM wait for every reader of the old ones. */
static void
hv_apply_range(int as, uint32_t lo, uint32_t hi)
{
    hv_slot_t     *slots = hv_slots[as];
    int            n     = hv_nslots[as];
    int            first;
    int            last;
    int            nr = 0;
    int            i;
    int            ro;
    uint8_t       *host;
    mem_mapping_t *dirty;
    uint32_t       xlo;
    uint32_t       xhi;
    uint32_t       off;

    if (hi > HV_GRANULES)
        hi = HV_GRANULES;
    if (lo >= hi)
        return;

    /* The slots that overlap the range are rebuilt whole: outside the range,
       they map what they mapped (the arrays there may be of the other SMM
       state). */
    xlo = lo;
    xhi = hi;
    for (i = 0; i < n; i++) {
        if ((slots[i].end > lo) && (slots[i].start < hi)) {
            if (slots[i].start < xlo)
                xlo = slots[i].start;
            if (slots[i].end > xhi)
                xhi = slots[i].end;
        }
    }

    for (uint32_t g = xlo; g < xhi; g++) {
        if ((g >= lo) && (g < hi)) {
            host = hv_resolve(as, g, &ro, &dirty);

            /* Shadow RAM that the BIOS keeps opening for writes and closing
               again stays read only: writes to it then go through the
               mappings, slower, but no slot changes, which cost far more.
               Read only is always right; writable is only faster. */
            if (host && (g < (0x100000 >> MEM_GRANULARITY_BITS))) {
                uint8_t       *flips = &hv_ro_flips[as][g >> (14 - MEM_GRANULARITY_BITS)];
                int            old_ro;
                mem_mapping_t *old_dirty;

                if (*flips >= 4)
                    ro = 1;
                else if (!(g & ((0x4000 >> MEM_GRANULARITY_BITS) - 1)) && (hv_slot_at(as, g, &old_ro, &old_dirty) == host) &&
                         (old_ro != ro))
                    (*flips)++;
            }
        } else
            host = hv_slot_at(as, g, &ro, &dirty);
        if (host == NULL)
            continue;
        if (nr && (hv_runs[nr - 1].end == g) && !hv_slot_boundary(g) && (hv_runs[nr - 1].ro == ro) &&
            (hv_runs[nr - 1].dirty == dirty) &&
            ((hv_runs[nr - 1].host + ((size_t) (g - hv_runs[nr - 1].start) << MEM_GRANULARITY_BITS)) == host)) {
            hv_runs[nr - 1].end++;
        } else {
            if (nr >= HV_MAX_SLOTS)
                fatal("KVM: too many memory slots\n");
            hv_runs[nr].start     = g;
            hv_runs[nr].end       = g + 1;
            hv_runs[nr].host      = host;
            hv_runs[nr].ro        = ro;
            hv_runs[nr].dirty     = dirty;
            hv_runs[nr].dirty_off = 0;
            if (dirty) {
                /* As the slot had it, outside the range. */
                off = (g << MEM_GRANULARITY_BITS) - dirty->base;
                hv_runs[nr].dirty_off = off;
            }
            nr++;
        }
    }

    /* The slots in the range are contiguous in the sorted list. */
    for (first = 0; (first < n) && (slots[first].end <= xlo); first++)
        ;
    for (last = first; (last < n) && (slots[last].start < xhi); last++)
        ;

    if ((last - first) == nr) {
        for (i = 0; i < nr; i++) {
            if ((slots[first + i].start != hv_runs[i].start) || (slots[first + i].end != hv_runs[i].end) ||
                (slots[first + i].host != hv_runs[i].host) || (slots[first + i].ro != hv_runs[i].ro) ||
                (slots[first + i].dirty != hv_runs[i].dirty))
                break;
        }
        if (i == nr)
            return; /* nothing changed */
    }

    if ((n - (last - first) + nr) > HV_MAX_SLOTS)
        fatal("KVM: too many memory slots\n");

    for (i = first; i < last; i++) {
        /* What was written to a logged slot since the last look. */
        if (slots[i].dirty)
            hv_dirty_slot(as, &slots[i]);
        hv_slot_ioctl(as, slots[i].id, slots[i].start, slots[i].start, slots[i].host, 0, 0);
        hv_slot_id_used[as][slots[i].id] = 0;
    }

    memmove(&slots[first + nr], &slots[last], (n - last) * sizeof(hv_slot_t));
    hv_nslots[as] = n - (last - first) + nr;

    for (i = 0; i < nr; i++) {
        int id;

        for (id = 0; hv_slot_id_used[as][id]; id++)
            ;
        hv_slot_id_used[as][id] = 1;
        hv_runs[i].id           = id;
        slots[first + i]        = hv_runs[i];
        hv_slot_ioctl(as, id, hv_runs[i].start, hv_runs[i].end, hv_runs[i].host, hv_runs[i].ro, !!hv_runs[i].dirty);
    }
}

/* The current view, for the current SMM state, into its address space. */
static void
hv_apply_view(uint64_t base, uint64_t size, uint32_t base_ignore)
{
    int      as = (hv_as_count > 1) ? !!in_smm : 0;
    uint64_t step;
    uint64_t end;
    uint64_t o;

    if (!size)
        return;

    end = base + size;
    if (base_ignore) {
        /* The range repeats at every alias. */
        step = ((uint64_t) (~base_ignore) & 0xffffffffULL) + 1ULL;
        for (o = 0; o <= base_ignore; o += step) {
            hv_apply_range(as, (uint32_t) ((base + o) >> MEM_GRANULARITY_BITS),
                           (uint32_t) ((end + o + MEM_GRANULARITY_MASK) >> MEM_GRANULARITY_BITS));
            if (!as)
                hv_apply_zones((uint32_t) ((base + o) >> MEM_GRANULARITY_BITS),
                               (uint32_t) MIN((end + o + MEM_GRANULARITY_MASK) >> MEM_GRANULARITY_BITS, HV_GRANULES));
        }
    } else {
        hv_apply_range(as, (uint32_t) (base >> MEM_GRANULARITY_BITS),
                       (uint32_t) ((end + MEM_GRANULARITY_MASK) >> MEM_GRANULARITY_BITS));
        if (!as)
            hv_apply_zones((uint32_t) (base >> MEM_GRANULARITY_BITS),
                           (uint32_t) MIN((end + MEM_GRANULARITY_MASK) >> MEM_GRANULARITY_BITS, HV_GRANULES));
    }

    /* The high memory area follows the first 64 KB while A20 is masked. */
    if ((base < 0x10000) && !mem_a20_state)
        hv_apply_range(as, 0x100000 >> MEM_GRANULARITY_BITS, 0x110000 >> MEM_GRANULARITY_BITS);
}

/* Changes to the memory map, applied to the slots when the guest is about
   to run again: a device often goes through states in between, which would
   each cost slot changes (VIA's register 63h disables every SMRAM range and
   then maps them again). */
#define HV_PENDING 64
static struct {
    uint64_t base;
    uint64_t size;
    uint32_t base_ignore;
} hv_pending[HV_PENDING];
static int hv_npending;
static int hv_pending_all;

void
hv_mem_changed(uint64_t base, uint64_t size, uint32_t base_ignore)
{
    int i;

    if ((vm_fd < 0) || hv_in_recalc || hv_pending_all || !size)
        return;

    for (i = 0; i < hv_npending; i++) {
        if ((hv_pending[i].base_ignore == base_ignore) && (base <= (hv_pending[i].base + hv_pending[i].size)) &&
            ((base + size) >= hv_pending[i].base)) {
            uint64_t end = MAX(base + size, hv_pending[i].base + hv_pending[i].size);

            hv_pending[i].base = MIN(base, hv_pending[i].base);
            hv_pending[i].size = end - hv_pending[i].base;
            return;
        }
    }

    if (hv_npending == HV_PENDING) {
        hv_pending_all = 1;
        return;
    }

    hv_pending[hv_npending].base        = base;
    hv_pending[hv_npending].size        = size;
    hv_pending[hv_npending].base_ignore = base_ignore;
    hv_npending++;
}

/* A range into both address spaces: the current SMM state's view from the
   arrays as they are, then the other's, from a recalculation for it, then
   the arrays back as they were. Memory that SMM does not change comes out
   the same and leaves the slots alone. */
static void
hv_mem_apply(uint64_t base, uint64_t size, uint32_t base_ignore)
{
    hv_apply_view(base, size, base_ignore);

    if (hv_as_count > 1) {
        in_smm = !in_smm;
        mem_mapping_recalc(base, size, base_ignore);
        hv_apply_view(base, size, base_ignore);
        in_smm = !in_smm;
        mem_mapping_recalc(base, size, base_ignore);
    }
}

static void
hv_mem_flush(void)
{
    if (!hv_npending && !hv_pending_all)
        return;

    hv_in_recalc = 1;
    if (hv_pending_all)
        hv_mem_apply(0x00000000ULL, 0x100000000ULL, 0);
    else {
        for (int i = 0; i < hv_npending; i++)
            hv_mem_apply(hv_pending[i].base, hv_pending[i].size, hv_pending[i].base_ignore);
    }
    hv_in_recalc   = 0;
    hv_npending    = 0;
    hv_pending_all = 0;
}

void
hv_a20_changed(void)
{
    if (vm_fd < 0)
        return;

    mem_mapping_recalc(0x100000, 0x10000, 0);
}

/* State. */
static inline void
hv_seg(struct kvm_segment *s, uint16_t sel, uint32_t base, uint32_t limit, uint8_t type, uint8_t code_data)
{
    memset(s, 0, sizeof(struct kvm_segment));
    s->selector = sel;
    s->base     = base;
    s->limit    = limit;
    s->type     = type;
    s->s        = code_data;
    s->present  = 1;
}

/* The processor's state after RESET or INIT, as reset_common() left it in
   cpu_state, into the virtual processor. */
static void
hv_apply_reset(void)
{
    struct kvm_regs        regs  = { 0 };
    struct kvm_sregs       sregs;
    struct kvm_vcpu_events ev;
    struct kvm_debugregs   dbg = { 0 };
    struct {
        struct kvm_msrs      hdr;
        struct kvm_msr_entry e[1];
    } msrs = { 0 };

    if (ioctl(cpu_fd, KVM_GET_SREGS, &sregs) < 0)
        fatal("KVM: GET_SREGS failed: %s\n", strerror(errno));

    regs.rflags = 0x00000002;
    regs.rip    = 0x0000fff0;
    regs.rdx    = EDX;

    hv_seg(&sregs.cs, 0xf000, 0xffff0000, 0xffff, 0x0b, 1);
    hv_seg(&sregs.ds, 0x0000, 0x00000000, 0xffff, 0x03, 1);
    hv_seg(&sregs.es, 0x0000, 0x00000000, 0xffff, 0x03, 1);
    hv_seg(&sregs.fs, 0x0000, 0x00000000, 0xffff, 0x03, 1);
    hv_seg(&sregs.gs, 0x0000, 0x00000000, 0xffff, 0x03, 1);
    hv_seg(&sregs.ss, 0x0000, 0x00000000, 0xffff, 0x03, 1);
    hv_seg(&sregs.ldt, 0x0000, 0x00000000, 0xffff, 0x02, 0);
    hv_seg(&sregs.tr, 0x0000, 0x00000000, 0xffff, 0x0b, 0);
    sregs.gdt.base  = 0;
    sregs.gdt.limit = 0xffff;
    sregs.idt.base  = 0;
    sregs.idt.limit = 0xffff;
    sregs.cr0       = cpu_state.CR0.l | 0x10;
    sregs.cr2       = 0;
    sregs.cr3       = 0;
    sregs.cr4       = 0;
    sregs.efer      = 0;
    memset(sregs.interrupt_bitmap, 0, sizeof(sregs.interrupt_bitmap));

    if (ioctl(cpu_fd, KVM_SET_REGS, &regs) < 0)
        fatal("KVM: SET_REGS failed: %s\n", strerror(errno));
    if (ioctl(cpu_fd, KVM_SET_SREGS, &sregs) < 0)
        fatal("KVM: SET_SREGS failed: %s\n", strerror(errno));

    dbg.dr6 = 0xffff0ff0;
    dbg.dr7 = 0x00000400;
    if (ioctl(cpu_fd, KVM_SET_DEBUGREGS, &dbg) < 0)
        fatal("KVM: SET_DEBUGREGS failed: %s\n", strerror(errno));

    /* Out of SMM, with nothing pending or being delivered. */
    if (ioctl(cpu_fd, KVM_GET_VCPU_EVENTS, &ev) < 0)
        fatal("KVM: GET_VCPU_EVENTS failed: %s\n", strerror(errno));
    memset(&ev.exception, 0, sizeof(ev.exception));
    memset(&ev.interrupt, 0, sizeof(ev.interrupt));
    ev.nmi.injected = ev.nmi.pending = ev.nmi.masked = 0;
    ev.smi.smm = ev.smi.pending = ev.smi.smm_inside_nmi = ev.smi.latched_init = 0;
    ev.flags = KVM_VCPUEVENT_VALID_NMI_PENDING | (hv_has_smm ? KVM_VCPUEVENT_VALID_SMM : 0);
    if (ioctl(cpu_fd, KVM_SET_VCPU_EVENTS, &ev) < 0)
        fatal("KVM: SET_VCPU_EVENTS failed: %s\n", strerror(errno));

    if (hv_reset_pending > 1) {
        /* RESET: the FPU as well, and SMBASE, which INIT leaves alone. */
        struct kvm_fpu fpu = { 0 };

        fpu.fcw   = 0x037f;
        fpu.mxcsr = 0x1f80;
        if (ioctl(cpu_fd, KVM_SET_FPU, &fpu) < 0)
            fatal("KVM: SET_FPU failed: %s\n", strerror(errno));

        if (hv_has_smm) {
            msrs.hdr.nmsrs  = 1;
            msrs.e[0].index = 0x0000009e; /* MSR_IA32_SMBASE */
            msrs.e[0].data  = smbase ? smbase : 0x30000;
            if (ioctl(cpu_fd, KVM_SET_MSRS, &msrs) != 1)
                pclog("KVM: could not reset SMBASE\n");
        }
    }

    run->request_interrupt_window = 0;
    hv_halted                     = 0;
    hv_smi_entering               = 0;
    hv_reset_pending              = 0;
}

void
hv_cpu_reset(int hard)
{
    /* Applied before the guest runs again: this comes from a port or a timer
       in the middle of an exit. */
    if (hard || (hv_reset_pending < 1))
        hv_reset_pending = hard ? 2 : 1;
}

static inline void
hv_get_segment(x86seg *dest, const struct kvm_segment *src)
{
    uint32_t limit20 = src->g ? (src->limit >> 12) : src->limit;

    dest->base       = src->base;
    dest->limit      = src->limit;
    dest->seg        = src->selector;
    dest->access     = (src->type & 0x0f) | (src->s ? 0x10 : 0x00) | ((src->dpl & 0x03) << 5) | (src->present ? 0x80 : 0);
    dest->limit_low  = limit20 & 0xffff;
    dest->limit_high = (limit20 >> 16) & 0xf;
    dest->ar_high    = (src->avl ? 0x10 : 0x00) | (src->l ? 0x20 : 0x00) | (src->db ? 0x40 : 0x00) | (src->g ? 0x80 : 0x00) | ((limit20 >> 16) & 0xf);
}

void
hv_sync_regs(void)
{
    struct kvm_regs  regs;
    struct kvm_sregs sregs;

    if ((cpu_fd < 0) || (ioctl(cpu_fd, KVM_GET_REGS, &regs) < 0) || (ioctl(cpu_fd, KVM_GET_SREGS, &sregs) < 0))
        return;

    EAX              = regs.rax;
    EBX              = regs.rbx;
    ECX              = regs.rcx;
    EDX              = regs.rdx;
    ESI              = regs.rsi;
    EDI              = regs.rdi;
    ESP              = regs.rsp;
    EBP              = regs.rbp;
    cpu_state.pc     = regs.rip;
    cpu_state.flags  = regs.rflags;
    cpu_state.eflags = regs.rflags >> 16;

    hv_get_segment(&cpu_state.seg_cs, &sregs.cs);
    hv_get_segment(&cpu_state.seg_ds, &sregs.ds);
    hv_get_segment(&cpu_state.seg_es, &sregs.es);
    hv_get_segment(&cpu_state.seg_ss, &sregs.ss);
    hv_get_segment(&cpu_state.seg_fs, &sregs.fs);
    hv_get_segment(&cpu_state.seg_gs, &sregs.gs);
    hv_get_segment(&tr, &sregs.tr);
    hv_get_segment(&ldt, &sregs.ldt);
    gdt.base  = sregs.gdt.base;
    gdt.limit = sregs.gdt.limit;
    idt.base  = sregs.idt.base;
    idt.limit = sregs.idt.limit;

    cpu_state.CR0.l = sregs.cr0;
    cr2             = sregs.cr2;
    cr3             = sregs.cr3;
    cr4             = sregs.cr4;
}

static void
hv_dump(const char *why)
{
    hv_sync_regs();
    pclog("KVM: %s at %04X:%08X (CR0 %08X)\n", why, CS, cpu_state.pc, cpu_state.CR0.l);
    pclog("KVM: EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X EBP=%08X ESP=%08X FLAGS=%04X%04X\n",
          EAX, EBX, ECX, EDX, ESI, EDI, EBP, ESP, cpu_state.eflags, cpu_state.flags);
    pclog("KVM: CS=%04X/%08X DS=%04X/%08X ES=%04X/%08X SS=%04X/%08X in_smm=%d\n",
          CS, cpu_state.seg_cs.base, DS, cpu_state.seg_ds.base, ES, cpu_state.seg_es.base,
          SS, cpu_state.seg_ss.base, in_smm);
}

/* SMM. */
static void
hv_smm_bus(int smm)
{
    if (smm) {
        in_smm = 1;
        smram_backup_all();
        smram_recalc_all(0);
        smi_latched = 0;
    } else {
        in_smm = 0;
        smram_recalc_all(1);
        nmi_mask = 1;
    }
}

/* Follow KVM's SMM state, which RSM leaves without stopping the guest. */
static inline void
hv_check_smm(void)
{
    int smm = !!(run->flags & KVM_RUN_X86_SMM);

    if (smm)
        hv_smi_entering = 0;
    else if (hv_smi_entering)
        return; /* the SMI has not been taken yet */

    if (smm != !!in_smm) {
        kvm_log("KVM: %s SMM\n", smm ? "entered" : "left");
        hv_smm_bus(smm);
    }
}

/* MSRs that KVM does not know: the emulated processor's. */
static void
hv_msr(int write)
{
    uint32_t old_eax = EAX;
    uint32_t old_ecx = ECX;
    uint32_t old_edx = EDX;
    uint32_t old_cr0 = cpu_state.CR0.l;
    int      old_abrt;

    old_abrt       = cpu_state.abrt;
    cpu_state.abrt = 0;
    cpu_state.CR0.l &= ~1; /* CPL 0, which KVM checked */
    ECX            = run->msr.index;
    if (write) {
        EAX = (uint32_t) run->msr.data;
        EDX = (uint32_t) (run->msr.data >> 32);
        cpu_WRMSR();
    } else {
        cpu_RDMSR();
        run->msr.data = EAX | ((uint64_t) EDX << 32);
    }
    run->msr.error = !!cpu_state.abrt;
    kvm_log("KVM: %sMSR %08X %016" PRIX64 "%s\n", write ? "WR" : "RD", run->msr.index, (uint64_t) run->msr.data,
            run->msr.error ? " #GP" : "");

    cpu_state.abrt = old_abrt;
    cpu_state.CR0.l = old_cr0;
    EAX            = old_eax;
    ECX            = old_ecx;
    EDX            = old_edx;
}

static void
hv_io(void)
{
    uint8_t *data = (uint8_t *) run + run->io.data_offset;


    for (uint32_t i = 0; i < run->io.count; i++, data += run->io.size) {
        if (run->io.direction == KVM_EXIT_IO_OUT) {
            switch (run->io.size) {
                case 1:
                    outb(run->io.port, *data);
                    break;
                case 2:
                    outw(run->io.port, *(uint16_t *) data);
                    break;
                default:
                    outl(run->io.port, *(uint32_t *) data);
                    break;
            }
        } else {
            switch (run->io.size) {
                case 1:
                    *data = inb(run->io.port);
                    break;
                case 2:
                    *(uint16_t *) data = inw(run->io.port);
                    break;
                default:
                    *(uint32_t *) data = inl(run->io.port);
                    break;
            }
        }
    }
}

/* The access as the processor makes it: the CPU's view of the memory map,
   which is not the bus masters' (mem_readb_phys()) where the chipset or a
   mapping tells them apart (SMRAM, shadow RAM write routing, CPU-only
   mappings). */
static void
hv_mmio(void)
{
    uint32_t addr = (uint32_t) run->mmio.phys_addr;
    uint8_t *data = run->mmio.data;
    uint32_t len  = run->mmio.len;
    uint32_t i;

    if (run->mmio.is_write) {
        if ((len == 1) || (len == 2) || (len == 4))
            mem_view_write(MEM_VIEW_CPU, addr, (len == 4) ? *(uint32_t *) data : ((len == 2) ? *(uint16_t *) data : data[0]), len);
        else if (len == 8) {
            mem_view_write(MEM_VIEW_CPU, addr, *(uint32_t *) data, 4);
            mem_view_write(MEM_VIEW_CPU, addr + 4, *(uint32_t *) &data[4], 4);
        } else {
            for (i = 0; i < len; i++)
                mem_view_write(MEM_VIEW_CPU, addr + i, data[i], 1);
        }
    } else {
        if (len == 4)
            *(uint32_t *) data = mem_view_read(MEM_VIEW_CPU, addr, 4);
        else if (len == 2)
            *(uint16_t *) data = mem_view_read(MEM_VIEW_CPU, addr, 2);
        else if (len == 8) {
            *(uint32_t *) data       = mem_view_read(MEM_VIEW_CPU, addr, 4);
            *(uint32_t *) &data[4] = mem_view_read(MEM_VIEW_CPU, addr + 4, 4);
        } else {
            for (i = 0; i < len; i++)
                data[i] = mem_view_read(MEM_VIEW_CPU, addr + i, 1);
        }
    }
}

static inline int
hv_nmi_pending(void)
{
    /* The processor's NMI blocking is KVM's; only the chipset's mask is
       86Box's. */
    return nmi && (nmi_mask || (nmi & 2));
}

/* SMIs, NMIs and interrupts for the next entry. */
static void
hv_events(void)
{
    if (smi_line) {
        if (hv_has_smm) {
            /* The SMRAM map has to be the SMM one before KVM writes the state
               save map into it, on the way into SMM at the next entry. */
            if (!in_smm) {
                hv_smm_bus(1);
                hv_smi_entering = 1;
            }
            if (ioctl(cpu_fd, KVM_SMI, 0) < 0)
                fatal("KVM: SMI failed: %s\n", strerror(errno));
        }
        smi_line = 0;
    }

    if (hv_nmi_pending()) {
        if (ioctl(cpu_fd, KVM_NMI, 0) < 0)
            fatal("KVM: NMI failed: %s\n", strerror(errno));
        nmi = 0;
    }

    run->request_interrupt_window = 0;
    if (x86_external_interrupt_pending()) {
        if (run->ready_for_interrupt_injection) {
            int vector = x86_external_interrupt_vector();

            if (vector != -1) {
                struct kvm_interrupt irq = { .irq = vector };

                if (ioctl(cpu_fd, KVM_INTERRUPT, &irq) < 0)
                    fatal("KVM: INTERRUPT failed: %s\n", strerror(errno));
            }
            /* Another one behind it. */
            if (x86_external_interrupt_pending())
                run->request_interrupt_window = 1;
        } else
            run->request_interrupt_window = 1;
    }
}

/* Wait out a HLT, until something wakes the processor or the call is up.
   Returns 0 if still halted. */
static int
hv_halt(uint64_t end)
{
    struct timespec ts;
    uint64_t        deadline;
    uint64_t        step;
    int64_t         rem;

    while (1) {
        if (hv_reset_pending || smi_line || hv_nmi_pending() || (run->if_flag && x86_external_interrupt_pending())) {
            hv_halted = 0;
            return 1;
        }

        /* Halted, the guest gets the time it is owed at once, up to its next
           timer at a time, which may wake it. */
        if (hv_lag_ns) {
            step = hv_lag_ns;
            if (timer_head) {
                rem = (int64_t) (timer_target - tsc);
                if (rem <= 0)
                    step = 0;
                else if ((((uint128_t) rem << 32) / hv_tsc_per_ns + 1) < step)
                    step = (uint64_t) (((uint128_t) rem << 32) / hv_tsc_per_ns) + 1;
            }
            hv_lag_ns -= step;
            hv_behind_ns -= step;
            hv_advance(step);
            hv_timers();
            continue;
        }

        hv_account_run(1);
        if ((int64_t) (hv_ns_last - end) >= 0)
            return 0;

        deadline = hv_stop_deadline();
        if ((int64_t) (deadline - end) > 0)
            deadline = end;
        if ((int64_t) (deadline - hv_ns_last) > 0) {
            ts.tv_sec  = (deadline - hv_ns_last) / 1000000000ULL;
            ts.tv_nsec = (deadline - hv_ns_last) % 1000000000ULL;
            if (sigtimedwait(&hv_sigset, NULL, &ts) > 0)
                hv_armed_ns = 0;
        }

        hv_account_run(1);
        hv_timers();
    }
}

static void
hv_set_cpuid(void)
{
    struct kvm_cpuid2 *sup;
    struct kvm_cpuid2 *cpuid;
    int                nsup = 64;
    uint32_t           sup1_ecx = 0, sup1_edx = 0, sup8_ecx = 0, sup8_edx = 0;
    uint32_t           old_eax = EAX, old_ebx = EBX, old_ecx = ECX, old_edx = EDX;
    uint32_t           limit0;
    uint32_t           limit8;
    int                n = 0;

    /* What KVM can give the guest on this host. */
    while (1) {
        sup       = calloc(1, sizeof(struct kvm_cpuid2) + (nsup * sizeof(struct kvm_cpuid_entry2)));
        sup->nent = nsup;
        if (ioctl(kvm_fd, KVM_GET_SUPPORTED_CPUID, sup) == 0)
            break;
        if (errno != E2BIG)
            fatal("KVM: GET_SUPPORTED_CPUID failed: %s\n", strerror(errno));
        free(sup);
        nsup *= 2;
    }
    for (uint32_t i = 0; i < sup->nent; i++) {
        if (sup->entries[i].function == 0x00000001) {
            sup1_ecx = sup->entries[i].ecx;
            sup1_edx = sup->entries[i].edx;
        } else if (sup->entries[i].function == 0x80000001) {
            sup8_ecx = sup->entries[i].ecx;
            sup8_edx = sup->entries[i].edx;
        }
    }
    free(sup);

    /* The emulated processor's, less what the host cannot do. */
    EAX = EBX = ECX = EDX = 0;
    cpu_CPUID();
    limit0 = EAX;
    EAX    = 0x80000000;
    EBX = ECX = EDX = 0;
    cpu_CPUID();
    limit8 = ((EAX & 0xffff0000) == 0x80000000) ? EAX : 0;
    if (limit0 > 0x1f)
        limit0 = 0x1f;
    if (limit8 > 0x80000020)
        limit8 = 0x80000020;

    cpuid = calloc(1, sizeof(struct kvm_cpuid2) + (64 * sizeof(struct kvm_cpuid_entry2)));
    for (uint32_t leaf = 0; ; leaf++) {
        if ((leaf > limit0) && (leaf < 0x80000000)) {
            if (!limit8)
                break;
            leaf = 0x80000000;
        }
        if ((leaf >= 0x80000000) && (leaf > limit8))
            break;

        EAX = leaf;
        EBX = ECX = EDX = 0;
        cpu_CPUID();
        if (leaf == 0x00000001) {
            ECX &= sup1_ecx;
            EDX &= sup1_edx;
        } else if (leaf == 0x80000001) {
            ECX &= sup8_ecx;
            EDX &= sup8_edx | (sup1_edx & 0x0183f3ff); /* the bits AMD repeats from leaf 1 */
        }
        cpuid->entries[n].function = leaf;
        cpuid->entries[n].eax      = EAX;
        cpuid->entries[n].ebx      = EBX;
        cpuid->entries[n].ecx      = ECX;
        cpuid->entries[n].edx      = EDX;
        pclog("KVM: CPUID %08X: %08X %08X %08X %08X\n", leaf, EAX, EBX, ECX, EDX);
        if (++n >= 64)
            break;
    }
    cpuid->nent = n;

    if (ioctl(cpu_fd, KVM_SET_CPUID2, cpuid) < 0)
        fatal("KVM: SET_CPUID2 failed: %s\n", strerror(errno));
    free(cpuid);

    EAX = old_eax;
    EBX = old_ebx;
    ECX = old_ecx;
    EDX = old_edx;
}

int
hv_available(void)
{
    int fd;

    if (hv_avail < 0) {
        fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
        hv_avail = (fd >= 0) && (ioctl(fd, KVM_GET_API_VERSION, 0) == KVM_API_VERSION) &&
                   (ioctl(fd, KVM_CHECK_EXTENSION, KVM_CAP_USER_MEMORY) > 0) &&
                   (ioctl(fd, KVM_CHECK_EXTENSION, KVM_CAP_READONLY_MEM) > 0) &&
                   (ioctl(fd, KVM_CHECK_EXTENSION, KVM_CAP_X86_USER_SPACE_MSR) > 0);
        if (fd >= 0)
            close(fd);
        if (!hv_avail)
            pclog("KVM: not available, the CPU is emulated\n");
    }

    return hv_avail;
}

static void
hv_init(void)
{
    struct kvm_enable_cap cap = { 0 };
    struct sigaction      sa  = { 0 };
    struct sigevent       sev = { 0 };
    sigset_t              mask;
    struct {
        struct kvm_signal_mask hdr;
        uint8_t                set[8];
    } kmask;

    kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm_fd < 0)
        fatal("KVM: Unable to open: %s\n", strerror(errno));

    vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
    if (vm_fd < 0)
        fatal("KVM: CREATE_VM failed: %s\n", strerror(errno));

    hv_has_smm  = ioctl(vm_fd, KVM_CHECK_EXTENSION, KVM_CAP_X86_SMM) > 0;
    hv_as_count = ioctl(vm_fd, KVM_CHECK_EXTENSION, KVM_CAP_MULTI_ADDRESS_SPACE);
    if (!hv_has_smm || (hv_as_count < 2)) {
        hv_has_smm  = 0;
        hv_as_count = 1;
    }
    pclog("KVM: initializing, SMM %s\n", hv_has_smm ? "supported" : "not supported");

    cap.cap = KVM_CAP_EXIT_ON_EMULATION_FAILURE;
    cap.args[0] = 1;
    ioctl(vm_fd, KVM_ENABLE_CAP, &cap);

    /* MSRs that KVM does not know are the emulated processor's. */
    memset(&cap, 0, sizeof(cap));
    cap.cap     = KVM_CAP_X86_USER_SPACE_MSR;
    cap.args[0] = KVM_MSR_EXIT_REASON_UNKNOWN;
    if (ioctl(vm_fd, KVM_ENABLE_CAP, &cap) < 0)
        fatal("KVM: ENABLE_CAP X86_USER_SPACE_MSR failed: %s\n", strerror(errno));

    cpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
    if (cpu_fd < 0)
        fatal("KVM: CREATE_VCPU failed: %s\n", strerror(errno));

    run_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    run      = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, cpu_fd, 0);
    if (run == MAP_FAILED)
        fatal("KVM: mmap failed: %s\n", strerror(errno));

    hv_set_cpuid();

    /* The ring of coalesced writes, a page into the run structure. */
    {
        int page = ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_COALESCED_MMIO);

        hv_nzones = 0;
        hv_ring   = NULL;
        if (page > 0) {
            hv_ring     = (struct kvm_coalesced_mmio_ring *) ((uint8_t *) run + (page * 4096));
            hv_ring_max = (4096 - sizeof(struct kvm_coalesced_mmio_ring)) / sizeof(struct kvm_coalesced_mmio);
        }
    }

    /* The whole memory map, in both address spaces. */
    memset(hv_nslots, 0, sizeof(hv_nslots));
    memset(hv_slot_id_used, 0, sizeof(hv_slot_id_used));
    hv_npending    = 0;
    hv_pending_all = 1;
    memset(hv_ro_flips, 0, sizeof(hv_ro_flips));

    /* The signal that stops the guest: blocked in this thread except while
       the guest runs, so it stays pending until KVM_RUN sees it. */
    hv_sig = SIGRTMIN + 5;
    sigemptyset(&hv_sigset);
    sigaddset(&hv_sigset, hv_sig);
    sa.sa_handler = hv_sig_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(hv_sig, &sa, NULL) < 0)
        fatal("KVM: sigaction failed: %s\n", strerror(errno));
    pthread_sigmask(SIG_BLOCK, &hv_sigset, NULL);
    pthread_sigmask(SIG_BLOCK, NULL, &mask);
    sigdelset(&mask, hv_sig);
    kmask.hdr.len = 8;
    memcpy(kmask.set, &mask, 8);
    if (ioctl(cpu_fd, KVM_SET_SIGNAL_MASK, &kmask) < 0)
        fatal("KVM: SET_SIGNAL_MASK failed: %s\n", strerror(errno));

    /* Aimed at this thread: a process directed timer signal can go to any
       thread, and does not stop KVM_RUN then. */
    hv_thread                  = pthread_self();
    sev.sigev_notify           = SIGEV_THREAD_ID;
    sev.sigev_signo            = hv_sig;
    sev.sigev_notify_thread_id = gettid();
    if (timer_create(CLOCK_MONOTONIC, &sev, &hv_timer) < 0)
        fatal("KVM: timer_create failed: %s\n", strerror(errno));
    hv_timer_valid = 1;
    hv_armed_ns    = 0;

    hv_ns_last     = hv_clock_ns();
    hv_lag_ns      = 0;
    hv_behind_ns   = 0;
    hv_tsc_frac    = 0;
    hv_tsc_applied = 0;
    hv_tsc_khz     = ioctl(cpu_fd, KVM_GET_TSC_KHZ, 0);
    {
        struct kvm_device_attr attr = {
            .group = KVM_VCPU_TSC_CTRL,
            .attr  = KVM_VCPU_TSC_OFFSET,
            .addr  = (uintptr_t) &hv_tsc_base
        };
        hv_tsc_attr = (hv_tsc_khz > 0) && (ioctl(cpu_fd, KVM_HAS_DEVICE_ATTR, &attr) == 0) &&
                      (ioctl(cpu_fd, KVM_GET_DEVICE_ATTR, &attr) == 0);
        if (!hv_tsc_attr)
            pclog("KVM: no TSC offset control, the guest's TSC runs on through stalls\n");
    }
    hv_cpuclock = 0.0;
    hv_rate_update();

    hv_halted        = 0;
    hv_smi_entering  = 0;
    hv_reset_pending = 2;
    hv_apply_reset();
    if (in_smm)
        hv_smm_bus(0);
}

void
hv_close(void)
{
    if (hv_timer_valid) {
        timer_delete(hv_timer);
        hv_timer_valid = 0;
        hv_drain_signal();
    }
    if (run != MAP_FAILED) {
        munmap(run, run_size);
        run = MAP_FAILED;
    }
    if (cpu_fd >= 0) {
        close(cpu_fd);
        cpu_fd = -1;
    }
    if (vm_fd >= 0) {
        close(vm_fd);
        vm_fd = -1;
    }
    if (kvm_fd >= 0) {
        close(kvm_fd);
        kvm_fd = -1;
    }
    hv_nslots[0] = hv_nslots[1] = 0;
}

static void
hv_stats(void)
{
#ifdef ENABLE_KVM_LOG
    static uint64_t next = 0;

    if ((int64_t) (hv_ns_last - next) < 0)
        return;
    if (next) {
        kvm_log("KVM: exits io %" PRIu64 " mmio %" PRIu64 " hlt %" PRIu64 " intr %" PRIu64 " irqwin %" PRIu64
                " msr %" PRIu64 " slot ops %" PRIu64 " exitreason %d tsc %" PRIu64 " target %" PRIu64 "\n",
                hv_stat_exits[KVM_EXIT_IO], hv_stat_exits[KVM_EXIT_MMIO], hv_stat_exits[KVM_EXIT_HLT],
                hv_stat_intr, hv_stat_exits[KVM_EXIT_IRQ_WINDOW_OPEN],
                hv_stat_exits[KVM_EXIT_X86_RDMSR] + hv_stat_exits[KVM_EXIT_X86_WRMSR], hv_stat_slot_ops, run->exit_reason, tsc, timer_target);
        kvm_log("KVM: coalesced writes %" PRIu64 "\n", hv_stat_coalesced);
        hv_stat_coalesced = 0;
        memset(hv_stat_exits, 0, sizeof(hv_stat_exits));
        hv_stat_intr = hv_stat_slot_ops = 0;
    }
    next = hv_ns_last + 5000000000ULL;
#endif
}

void
hv_exec(UNUSED(int32_t cycs))
{
    uint64_t end;
    uint64_t deadline;
    int      ret;

    if (cpu_fd < 0)
        hv_init();

    hv_rate_update();

    /* The time since the last call, in the main loop or paused, is lag. */
    hv_account_out();
    end = hv_ns_last + (force_10ms ? (10 * HV_QUANTUM_NS) : HV_QUANTUM_NS);

    /* What the guest wrote into a device's memory since the last call, for
       the device to see before its timers next look. */
    hv_dirty_all();

    while (1) {
        hv_account_out();
        hv_timers();

        /* A reset some devices leave for the CPU loop (pci_trc_reset()). */
        if (cpu_init) {
            cpu_init = 0;
            resetx86();
        }

        if (hv_reset_pending)
            hv_apply_reset();

        if (hv_halted && !hv_halt(end))
            break;

        if ((int64_t) (hv_ns_last - end) >= 0)
            break;

        hv_events();
        hv_mem_flush();

        /* Everything up to here, the slot changes above included, was time
           outside the guest. */
        hv_account_out();
        deadline = hv_stop_deadline();
        if ((int64_t) (deadline - end) > 0)
            deadline = end;
        hv_arm(deadline);
        hv_tsc_sync();

        /* Only cycles of devices, not of the processor, are counted now. */
        cycles = 0;

        ret = ioctl(cpu_fd, KVM_RUN, 0);
        hv_account_run(0);
        hv_drain();

        if (ret < 0) {
            if ((errno != EINTR) && (errno != EAGAIN))
                fatal("KVM: RUN failed: %s\n", strerror(errno));
            hv_drain_signal();
            hv_armed_ns = 0;
            hv_stat_intr++;
            hv_check_smm();
            hv_stats();
            continue;
        }

        hv_check_smm();

        /* The devices as they are at the time of the access. */
        hv_timers();

        if (run->exit_reason < 64)
            hv_stat_exits[run->exit_reason]++;

        switch (run->exit_reason) {
            case KVM_EXIT_IO:
                hv_io();
                break;

            case KVM_EXIT_MMIO:
                hv_mmio();
                break;

            case KVM_EXIT_HLT:
                hv_halted = 1;
                break;

            case KVM_EXIT_IRQ_WINDOW_OPEN:
            case KVM_EXIT_INTR:
                break;

            case KVM_EXIT_X86_RDMSR:
                hv_msr(0);
                break;

            case KVM_EXIT_X86_WRMSR:
                hv_msr(1);
                break;

            case KVM_EXIT_SHUTDOWN:
                /* Triple fault. */
                hv_dump("triple fault");
                softresetx86();
                cpu_set_edx();
                break;

            case KVM_EXIT_INTERNAL_ERROR:
                hv_dump("internal error");
                for (uint32_t i = 0; i < run->internal.ndata; i++)
                    pclog("KVM: data[%d] = %016" PRIx64 "\n", i, (uint64_t) run->internal.data[i]);
                fatal("KVM: Internal error %d\n", run->internal.suberror);
                break;

            case KVM_EXIT_FAIL_ENTRY:
                hv_dump("entry failure");
                fatal("KVM: Entry failure %016" PRIX64 "\n", (uint64_t) run->fail_entry.hardware_entry_failure_reason);
                break;

            default:
                hv_dump("unknown exit");
                fatal("KVM: Unknown exit %d\n", run->exit_reason);
                break;
        }

        hv_stats();
    }
}
