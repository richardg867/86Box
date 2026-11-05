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
#include <sys/resource.h>
#include <linux/kvm.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include "x86.h"
#include "x86_interrupt.h"
#include "x87_sf.h"
#include "x87.h"
#include "x86seg.h"
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
#define HV_PIN_FLIPS 4    /* flips before it is kept read only */
#define HV_PIN_NEVER 0xff /* KVM could not deliver an event into it */
static int       hv_pin_ok;
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
static int hv_smi_halted; /* the SMI ended a HLT */

/* Statistics, logged every few seconds with ENABLE_KVM_LOG. */
static uint64_t hv_stat_exits[64];
static uint64_t hv_stat_intr;
static uint64_t hv_stat_slot_ops;
static uint64_t hv_stat_emulated;
static uint64_t hv_stat_emulated_next = 10;
static uint64_t hv_stat_ferr;

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

/* The high memory area follows A20 (DOS toggles it around every call into
   its kernel there, a thousand times a second as it boots) without slot
   changes, each of which costs some 40 us: its slot maps a window of host
   memory that is made to show either the first 64 KB of RAM or its own, by
   remapping. Both have to be shared memory for that, so the first 1088 KB
   of RAM are replaced with shared memory. */
#define HV_HMA      0x100000
#define HV_HMA_SIZE 0x10000
static uint8_t *hv_hma_win  = NULL;
static uint8_t *hv_hma_ram  = NULL; /* the RAM block it was made for */
static int      hv_hma_low  = -1;   /* it shows the first 64 KB */

static void
hv_hma_point(int low)
{
    if (low == hv_hma_low)
        return;
    if (mremap(hv_hma_ram + (low ? 0 : HV_HMA), 0, HV_HMA_SIZE, MREMAP_MAYMOVE | MREMAP_FIXED, hv_hma_win) == MAP_FAILED)
        fatal("KVM: remapping the high memory area failed: %s\n", strerror(errno));
    hv_hma_low = low;
}

static int
hv_hma_setup(void)
{
    uint8_t *save;
    void    *p;

    if (hv_hma_ram == ram)
        return hv_hma_win != NULL;

    hv_hma_ram = ram;
    hv_hma_low = -1;
    if (hv_hma_win != NULL) {
        munmap(hv_hma_win, HV_HMA_SIZE);
        hv_hma_win = NULL;
    }
    if ((ram == NULL) || ((uintptr_t) ram & 0xfff) || (((uint64_t) mem_size << 10) < (HV_HMA + HV_HMA_SIZE)))
        return 0;

    save = malloc(HV_HMA + HV_HMA_SIZE);
    memcpy(save, ram, HV_HMA + HV_HMA_SIZE);
    p = mmap(ram, HV_HMA + HV_HMA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED)
        fatal("KVM: making low memory shared failed: %s\n", strerror(errno));
    memcpy(ram, save, HV_HMA + HV_HMA_SIZE);
    free(save);

    hv_hma_win = mmap(NULL, HV_HMA_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (hv_hma_win == MAP_FAILED) {
        hv_hma_win = NULL;
        return 0;
    }
    hv_hma_point(!mem_a20_state);
    return 1;
}

/* Memory slots. */
static uint8_t *
hv_resolve(int as, uint32_t g, int *ro, mem_mapping_t **dirty)
{
    int      a20 = mem_a20_state || (as && unmask_a20_in_smm);
    int      writable;
    uint8_t *ret;

    /* With A20 masked, the high memory area is the first 64 KB again. Only
       that, which real mode code can reach, is followed: the rest of memory
       would be thousands of slot changes at every toggle. */
    if ((g >= (HV_HMA >> MEM_GRANULARITY_BITS)) && (g < ((HV_HMA + HV_HMA_SIZE) >> MEM_GRANULARITY_BITS))) {
        uint32_t lo = g - (HV_HMA >> MEM_GRANULARITY_BITS);
        int      lo_writable;
        uint8_t *lo_ret = mem_granule_direct(lo, &lo_writable, dirty);

        ret = mem_granule_direct(g, &writable, dirty);
        /* Both RAM, as they are: the window, whichever it shows. */
        if (!(as && unmask_a20_in_smm) && hv_hma_setup() && ret && lo_ret && writable && lo_writable && !*dirty &&
            (ret == (ram + (g << MEM_GRANULARITY_BITS))) && (lo_ret == (ram + (lo << MEM_GRANULARITY_BITS)))) {
            *ro = 0;
            return hv_hma_win + (lo << MEM_GRANULARITY_BITS);
        }
        if (!a20)
            g = lo;
    }

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
               Only while paging is off, before an operating system takes
               over memory: KVM cannot push an exception frame, switch tasks
               or run every instruction on memory that is not mapped
               writable, and an operating system may use that RAM for
               anything (hv_pin_poll(), hv_pin_failed()). */
            if (host && (g < (0x100000 >> MEM_GRANULARITY_BITS))) {
                uint8_t       *flips = &hv_ro_flips[as][g >> (14 - MEM_GRANULARITY_BITS)];
                int            old_ro;
                mem_mapping_t *old_dirty;

                if (hv_pin_ok && (*flips >= HV_PIN_FLIPS) && (*flips != HV_PIN_NEVER))
                    ro = 1;
                else if (!(g & ((0x4000 >> MEM_GRANULARITY_BITS) - 1)) && (hv_slot_at(as, g, &old_ro, &old_dirty) == host) &&
                         (old_ro != ro) && (*flips < HV_PIN_FLIPS))
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

    /* The window is pointed at what A20 makes the area; if it is not in use
       the slots there change. */
    if (hv_hma_win && (hv_hma_ram == ram))
        hv_hma_point(!mem_a20_state);
    mem_mapping_recalc(HV_HMA, HV_HMA_SIZE, 0);
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

/* A segment register as KVM holds it, into the CPU emulation's form: the
   bounds an access is checked against, and a null selector's mark. */
static void
hv_get_segment(x86seg *dest, const struct kvm_segment *src, int pm)
{
    uint32_t limit20 = src->g ? (src->limit >> 12) : src->limit;

    dest->base    = src->base;
    dest->limit   = src->limit;
    dest->seg     = src->selector;
    dest->access  = (src->type & 0x0f) | (src->s ? 0x10 : 0x00) | ((src->dpl & 0x03) << 5) | (src->present ? 0x80 : 0);
    dest->ar_high = (src->avl ? 0x10 : 0x00) | (src->db ? 0x40 : 0x00) | (src->g ? 0x80 : 0x00) | ((limit20 >> 16) & 0xf);
    dest->checked = 1;
    if (src->s && ((src->type & 0x0c) == 0x04)) {
        /* Expand-down data. */
        dest->limit_low  = src->limit + 1;
        dest->limit_high = src->db ? 0xffffffff : 0xffff;
    } else {
        dest->limit_low  = 0;
        dest->limit_high = src->limit;
    }
    if (pm && src->unusable && !(src->selector & 0xfffc)) {
        dest->access  = 0x80;
        dest->ar_high = 0x10;
        dest->base    = 0xffffffff;
        dest->checked = 0;
    }
}

/* And back: a segment register the CPU emulation loaded. */
static void
hv_put_segment(struct kvm_segment *dest, const x86seg *src, int pm)
{
    memset(dest, 0, sizeof(struct kvm_segment));
    dest->selector = src->seg;
    dest->type     = src->access & 0x0f;
    dest->s        = !!(src->access & 0x10);
    dest->dpl      = (src->access >> 5) & 3;
    dest->present  = !!(src->access & 0x80);
    dest->avl      = !!(src->ar_high & 0x10);
    dest->db       = !!(src->ar_high & 0x40);
    dest->g        = !!(src->ar_high & 0x80);
    if (pm && (src->base == 0xffffffff) && !(src->seg & 0xfffc)) {
        dest->unusable = 1;
        dest->present  = 0;
        return;
    }
    dest->base  = src->base;
    dest->limit = src->limit;
}

static inline int
hv_segment_same(const x86seg *a, const x86seg *b)
{
    return (a->base == b->base) && (a->limit == b->limit) && (a->seg == b->seg) && (a->access == b->access) &&
           (a->ar_high == b->ar_high);
}

/* The CPU emulation's state, the segment registers' mode dependent forms
   with it, from KVM's. */
static void
hv_load_state(const struct kvm_regs *regs, const struct kvm_sregs *sregs)
{
    int pm;
    int v86;

    EAX              = regs->rax;
    EBX              = regs->rbx;
    ECX              = regs->rcx;
    EDX              = regs->rdx;
    ESI              = regs->rsi;
    EDI              = regs->rdi;
    ESP              = regs->rsp;
    EBP              = regs->rbp;
    cpu_state.pc     = regs->rip;
    cpu_state.flags  = regs->rflags & 0xffff;
    cpu_state.eflags = (regs->rflags >> 16) & 0xffff;

    cpu_state.CR0.l = sregs->cr0;
    cr2             = sregs->cr2;
    cr3             = sregs->cr3;
    cr4             = sregs->cr4;
    pm              = sregs->cr0 & 1;
    v86             = pm && (cpu_state.eflags & VM_FLAG);

    hv_get_segment(&cpu_state.seg_cs, &sregs->cs, pm && !v86);
    hv_get_segment(&cpu_state.seg_ds, &sregs->ds, pm && !v86);
    hv_get_segment(&cpu_state.seg_es, &sregs->es, pm && !v86);
    hv_get_segment(&cpu_state.seg_ss, &sregs->ss, pm && !v86);
    hv_get_segment(&cpu_state.seg_fs, &sregs->fs, pm && !v86);
    hv_get_segment(&cpu_state.seg_gs, &sregs->gs, pm && !v86);
    hv_get_segment(&tr, &sregs->tr, 0);
    hv_get_segment(&ldt, &sregs->ldt, 0);
    gdt.base  = sregs->gdt.base;
    gdt.limit = sregs->gdt.limit;
    idt.base  = sregs->idt.base;
    idt.limit = sregs->idt.limit;

    /* VMX takes the privilege level from SS, as a conforming code segment's
       DPL is not it; the CPU emulation from CS. */
    cpu_state.seg_cs.access = (cpu_state.seg_cs.access & ~0x60) | ((sregs->ss.dpl & 3) << 5);

    cpu_cur_status = (pm ? CPU_STATUS_PMODE : 0) | (v86 ? CPU_STATUS_V86 : 0) | (in_smm ? CPU_STATUS_SMM : 0);
    if (!pm || v86 || cpu_state.seg_ds.base || cpu_state.seg_ds.limit_low || (cpu_state.seg_ds.limit_high != 0xffffffff))
        cpu_cur_status |= CPU_STATUS_NOTFLATDS | CPU_STATUS_NOTWIDEDS;
    if (!pm || v86 || cpu_state.seg_es.base || cpu_state.seg_es.limit_low || (cpu_state.seg_es.limit_high != 0xffffffff))
        cpu_cur_status |= CPU_STATUS_NOTFLATES;
    if (!pm || v86 || cpu_state.seg_ss.base || cpu_state.seg_ss.limit_low || (cpu_state.seg_ss.limit_high != 0xffffffff))
        cpu_cur_status |= CPU_STATUS_NOTFLATSS | CPU_STATUS_NOTWIDESS;
    use32   = (pm && !v86 && sregs->cs.db) ? 0x300 : 0;
    stack32 = pm && !v86 && sregs->ss.db;
    if (use32)
        cpu_cur_status |= CPU_STATUS_USE32;
    if (stack32)
        cpu_cur_status |= CPU_STATUS_STACK32;
}

void
hv_sync_regs(void)
{
    struct kvm_regs  regs;
    struct kvm_sregs sregs;

    if ((cpu_fd < 0) || (ioctl(cpu_fd, KVM_GET_REGS, &regs) < 0) || (ioctl(cpu_fd, KVM_GET_SREGS, &sregs) < 0))
        return;

    hv_load_state(&regs, &sregs);
}

/* KVM's processor state, loaded into the CPU emulation's to work on, and
   what changed of it put back. */
typedef struct hv_state_t {
    struct kvm_regs  regs;
    struct kvm_sregs sregs;
    struct kvm_fpu   fpu;
    x86seg           old[8];
    uint32_t         old_cr[4];
    uint32_t         old_dt[4];
} hv_state_t;

static x86seg *const hv_state_segs[8] = { &cpu_state.seg_cs, &cpu_state.seg_ds, &cpu_state.seg_es, &cpu_state.seg_ss,
                                          &cpu_state.seg_fs, &cpu_state.seg_gs, &tr, &ldt };

static void
hv_state_begin(hv_state_t *st)
{
    if ((ioctl(cpu_fd, KVM_GET_REGS, &st->regs) < 0) || (ioctl(cpu_fd, KVM_GET_SREGS, &st->sregs) < 0) ||
        (ioctl(cpu_fd, KVM_GET_FPU, &st->fpu) < 0))
        fatal("KVM: getting the processor state failed: %s\n", strerror(errno));

    hv_load_state(&st->regs, &st->sregs);
    for (int i = 0; i < 8; i++)
        st->old[i] = *hv_state_segs[i];
    st->old_cr[0] = cpu_state.CR0.l;
    st->old_cr[1] = cr2;
    st->old_cr[2] = cr3;
    st->old_cr[3] = cr4;
    st->old_dt[0] = gdt.base;
    st->old_dt[1] = gdt.limit;
    st->old_dt[2] = idt.base;
    st->old_dt[3] = idt.limit;

    /* The softfloat FPU's registers are KVM's as they are. */
    fpu_state.cwd = st->fpu.fcw;
    fpu_state.swd = st->fpu.fsw;
    fpu_state.tos = (st->fpu.fsw >> 11) & 7;
    fpu_state.foo = st->fpu.last_opcode & 0x7ff;
    fpu_state.fip = st->fpu.last_ip;
    fpu_state.fdp = st->fpu.last_dp;
    for (int i = 0; i < 8; i++) {
        floatx80 *r = &fpu_state.st_space[(fpu_state.tos + i) & 7];

        memcpy(&r->signif, &st->fpu.fpr[i][0], 8);
        memcpy(&r->signExp, &st->fpu.fpr[i][8], 2);
    }
    fpu_state.tag = unpack_FPU_TW(st->fpu.ftwx);

    /* The guest's page tables, not those the last lookups were of. */
    flushmmucache();
}

static void
hv_state_end(hv_state_t *st)
{
    struct kvm_segment *ksegs[8] = { &st->sregs.cs, &st->sregs.ds, &st->sregs.es, &st->sregs.ss,
                                     &st->sregs.fs, &st->sregs.gs, &st->sregs.tr, &st->sregs.ldt };
    int                 pm       = cpu_state.CR0.l & 1;
    int                 changed  = 0;

    st->regs.rax    = EAX;
    st->regs.rbx    = EBX;
    st->regs.rcx    = ECX;
    st->regs.rdx    = EDX;
    st->regs.rsi    = ESI;
    st->regs.rdi    = EDI;
    st->regs.rsp    = ESP;
    st->regs.rbp    = EBP;
    st->regs.rip    = cpu_state.pc;
    st->regs.rflags = cpu_state.flags | ((uint32_t) cpu_state.eflags << 16) | 2;

    /* The segments and system registers only where they changed. */
    for (int i = 0; i < 8; i++) {
        if (!hv_segment_same(hv_state_segs[i], &st->old[i])) {
            hv_put_segment(ksegs[i], hv_state_segs[i], pm && !(cpu_state.eflags & VM_FLAG) && (i < 6));
            changed = 1;
        }
    }
    if ((st->old_cr[0] != cpu_state.CR0.l) || (st->old_cr[1] != cr2) || (st->old_cr[2] != cr3) ||
        (st->old_cr[3] != cr4) || (st->old_dt[0] != gdt.base) || (st->old_dt[1] != gdt.limit) ||
        (st->old_dt[2] != idt.base) || (st->old_dt[3] != idt.limit)) {
        st->sregs.cr0       = cpu_state.CR0.l;
        st->sregs.cr2       = cr2;
        st->sregs.cr3       = cr3;
        st->sregs.cr4       = cr4;
        st->sregs.gdt.base  = gdt.base;
        st->sregs.gdt.limit = gdt.limit;
        st->sregs.idt.base  = idt.base;
        st->sregs.idt.limit = idt.limit;
        changed             = 1;
    }

    st->fpu.fcw         = fpu_state.cwd;
    st->fpu.fsw         = (fpu_state.swd & ~0x3800) | ((fpu_state.tos & 7) << 11);
    st->fpu.ftwx        = pack_FPU_TW(fpu_state.tag);
    st->fpu.last_opcode = fpu_state.foo;
    st->fpu.last_ip     = fpu_state.fip;
    st->fpu.last_dp     = fpu_state.fdp;
    for (int i = 0; i < 8; i++) {
        const floatx80 *r = &fpu_state.st_space[(fpu_state.tos + i) & 7];

        memset(st->fpu.fpr[i], 0, 16);
        memcpy(&st->fpu.fpr[i][0], &r->signif, 8);
        memcpy(&st->fpu.fpr[i][8], &r->signExp, 2);
    }

    if ((ioctl(cpu_fd, KVM_SET_REGS, &st->regs) < 0) || (changed && (ioctl(cpu_fd, KVM_SET_SREGS, &st->sregs) < 0)) ||
        (ioctl(cpu_fd, KVM_SET_FPU, &st->fpu) < 0))
        fatal("KVM: setting the processor state failed: %s\n", strerror(errno));
}

/* An instruction KVM could not emulate, an MMX or x87 one whose memory
   operand is a device's, run by the CPU interpreter on KVM's state. */
static void
hv_emulate(void)
{
    hv_state_t st;

    hv_state_begin(&st);
    /* The first few, and how many there have been every power of ten. */
    hv_stat_emulated++;
    if ((hv_stat_emulated <= 4) || (hv_stat_emulated == hv_stat_emulated_next)) {
        pclog("KVM: interpreting the instruction at %04X:%08X, which KVM could not emulate (%" PRIu64 " so far)\n",
              CS, cpu_state.pc, hv_stat_emulated);
        if (hv_stat_emulated >= hv_stat_emulated_next)
            hv_stat_emulated_next *= 10;
    }
    exec386_hv_step();
    hv_state_end(&st);
}

/* Guest memory by linear address, through its page tables as KVM has them
   (no PAE), the CPU's view of the memory map. Returns 0 if not present. */
static int
hv_lin_readl(const struct kvm_sregs *sr, uint32_t lin, uint32_t *val)
{
    uint32_t phys = lin;
    uint32_t e;

    if (sr->cr0 & 0x80000000) {
        if (sr->cr4 & 0x20)
            return 0;
        e = mem_view_read(MEM_VIEW_CPU, (sr->cr3 & 0xfffff000) + ((lin >> 22) << 2), 4);
        if (!(e & 1))
            return 0;
        if ((e & 0x80) && (sr->cr4 & 0x10))
            phys = (e & 0xffc00000) | (lin & 0x003fffff);
        else {
            e = mem_view_read(MEM_VIEW_CPU, (e & 0xfffff000) + (((lin >> 12) & 0x3ff) << 2), 4);
            if (!(e & 1))
                return 0;
            phys = (e & 0xfffff000) | (lin & 0xfff);
        }
    }
    *val = mem_view_read(MEM_VIEW_CPU, phys, 4);
    return 1;
}

/* Instruction breakpoints of KVM's own (KVM_GUESTDBG_USE_HW_BP), which stop
   the guest at a linear address. The guest's own debug registers do not
   work while one is set, nor does its single stepping, which is handed
   back to it (KVM_GUESTDBG_INJECT_DB). */
#define HV_BP_FERR    0 /* the vector 16 handler while CR0.NE is clear */
#define HV_BP_SMM     1 /* the SMM handler's entry, an SMI taken halted */
#define HV_BP_SMM_RET 2 /* where its RSM returns */
static uint32_t hv_bp_addr[4];
static uint8_t  hv_bp_on[4];
static int      hv_bp_dirty;

static void
hv_bp(int n, int on, uint32_t addr)
{
    if ((hv_bp_on[n] != on) || (on && (hv_bp_addr[n] != addr))) {
        hv_bp_on[n]   = on;
        hv_bp_addr[n] = addr;
        hv_bp_dirty   = 1;
    }
}

static void
hv_bp_apply(int inject_db)
{
    struct kvm_guest_debug dbg;
    int                    any = 0;

    if (!hv_bp_dirty && !inject_db)
        return;

    memset(&dbg, 0, sizeof(dbg));
    dbg.arch.debugreg[7] = 0x400;
    for (int i = 0; i < 4; i++) {
        if (hv_bp_on[i]) {
            dbg.arch.debugreg[i] = hv_bp_addr[i];
            dbg.arch.debugreg[7] |= 2ULL << (i * 2); /* Gn, an instruction breakpoint */
            any = 1;
        }
    }
    if (any || inject_db)
        dbg.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_HW_BP | (inject_db ? KVM_GUESTDBG_INJECT_DB : 0);
    if (ioctl(cpu_fd, KVM_SET_GUEST_DEBUG, &dbg) < 0)
        fatal("KVM: SET_GUEST_DEBUG failed: %s\n", strerror(errno));
    hv_bp_dirty = 0;
}

/* Go on past a breakpoint the guest is stopped at. */
static void
hv_bp_resume(void)
{
    struct kvm_regs regs;

    if ((ioctl(cpu_fd, KVM_GET_REGS, &regs) == 0)) {
        regs.rflags |= 0x10000; /* RF */
        ioctl(cpu_fd, KVM_SET_REGS, &regs);
    }
}

/* Pinned shadow RAM goes back to what the chipset makes it as the guest
   turns paging on. */
static void
hv_pin_poll(const struct kvm_sregs *sr)
{
    int ok = !(sr->cr0 & 0x80000000);

    if (ok == hv_pin_ok)
        return;
    hv_pin_ok = ok;
    if (!ok)
        hv_mem_changed(0, 0x100000, 0);
}

/* KVM could not deliver an event (an exception's frame, a task switch)
   into memory at gpa. Returns 1 if it was pinned shadow RAM, which is
   mapped as it is from now on and the event delivered again. */
static int
hv_pin_failed(uint64_t gpa)
{
    uint32_t chunk = (uint32_t) gpa >> 14;

    if ((gpa >= 0x100000) || ((hv_ro_flips[0][chunk] < HV_PIN_FLIPS) && (hv_ro_flips[1][chunk] < HV_PIN_FLIPS)) ||
        ((hv_ro_flips[0][chunk] == HV_PIN_NEVER) && (hv_ro_flips[1][chunk] == HV_PIN_NEVER)))
        return 0;

    hv_ro_flips[0][chunk] = hv_ro_flips[1][chunk] = HV_PIN_NEVER;
    hv_mem_changed(chunk << 14, 0x4000, 0);
    return 1;
}

/* FERR#. With CR0.NE clear, the x87 reports an error on FERR#, which the
   board turns into IRQ 13 (x87_ferr.c), and IGNNE# lets it go on past it;
   VMX keeps CR0.NE set, so the processor raises #MF instead, the guest's
   vector 16, a BIOS's INT 10h or Windows 9x's reflection of it. The guest
   is stopped where it enters that handler, the #MF undone, and the
   instruction run by the CPU interpreter, which reports the error as the
   board does, or with IGNNE# runs it. Until the error is taken, the
   processor waits at the instruction for an interrupt. */
static void
hv_poll(void)
{
    struct kvm_sregs sr;
    struct kvm_fpu   fpu;
    uint32_t         lo;
    uint32_t         hi;
    uint32_t         d0;
    uint32_t         d1;
    uint32_t         sel;
    uint32_t         base;
    uint32_t         off;
    uint32_t         lin = 0;
    int              on  = 0;

    /* FERR# goes down, IGNNE# with it, as ES clears. */
    if (fpu_ferr && (ioctl(cpu_fd, KVM_GET_FPU, &fpu) == 0) && !(fpu.fsw & 0x80))
        x87_ferr_clear();

    if (ioctl(cpu_fd, KVM_GET_SREGS, &sr) < 0)
        goto done;
    hv_pin_poll(&sr);
    if ((fpu_type == FPU_NONE) || (sr.cr0 & 0x20))
        goto done;

    if (!(sr.cr0 & 1)) {
        if ((sr.idt.limit >= 0x43) && hv_lin_readl(&sr, sr.idt.base + 0x40, &lo)) {
            lin = ((lo >> 16) << 4) + (lo & 0xffff);
            on  = 1;
        }
        goto done;
    }

    if ((sr.idt.limit < 0x87) || !hv_lin_readl(&sr, sr.idt.base + 0x80, &lo) || !hv_lin_readl(&sr, sr.idt.base + 0x84, &hi))
        goto done;
    if (!(hi & 0x8000) || (((hi >> 8) & 0x16) != 0x06)) /* present interrupt or trap gate */
        goto done;
    sel = lo >> 16;
    off = (lo & 0xffff) | ((hi & 0x800) ? (hi & 0xffff0000) : 0);
    if (sel & 4) {
        if (((sel & ~7) + 7) > sr.ldt.limit)
            goto done;
        base = sr.ldt.base;
    } else {
        if (((sel & ~7) + 7) > sr.gdt.limit)
            goto done;
        base = sr.gdt.base;
    }
    if (!hv_lin_readl(&sr, base + (sel & ~7), &d0) || !hv_lin_readl(&sr, base + (sel & ~7) + 4, &d1))
        goto done;
    lin = ((d0 >> 16) | ((d1 & 0xff) << 16) | (d1 & 0xff000000)) + off;
    on  = 1;

done:
    hv_bp(HV_BP_FERR, on, lin);
}

/* The FPU as FERR# last went up or an error was let by (IGNNE#). ES
   clears without stopping the guest (FNCLEX, FNINIT, FNSAVE), which lowers
   FERR# and the board's IGNNE# latch; with ES set no waiting instruction
   runs, so an FPU that is no longer as it was then has had its error
   cleared since, and the one it now has is a new one. */
static struct kvm_fpu hv_ferr_snap;

static int
hv_ferr_same(const struct kvm_fpu *a, const struct kvm_fpu *b)
{
    if ((a->fcw != b->fcw) || (a->fsw != b->fsw) || (a->ftwx != b->ftwx) || (a->last_opcode != b->last_opcode) ||
        (a->last_ip != b->last_ip) || (a->last_dp != b->last_dp))
        return 0;
    for (int i = 0; i < 8; i++) {
        if (memcmp(a->fpr[i], b->fpr[i], 10))
            return 0;
    }
    return 1;
}

/* At the vector 16 handler: an #MF, if the instruction it was raised at is
   an x87 or MMX one with an error pending, not a software INT 10h. */
static void
hv_ferr_hit(void)
{
    hv_state_t st;
    uint32_t   sp = 0;
    uint32_t   ip = 0;
    uint32_t   rcs = 0;
    uint32_t   fl = 0;
    uint32_t   ret = 0;
    uint32_t   pc;
    uint8_t    b = 0;
    int        is32 = 0;
    int        n;

    if (in_smm) {
        hv_bp_resume();
        return;
    }

    hv_state_begin(&st);
    if (!(st.fpu.fsw & 0x80) || !(st.fpu.fsw & ~st.fpu.fcw & 0x3f))
        goto not_mf;
    if (fpu_ferr && !hv_ferr_same(&st.fpu, &hv_ferr_snap))
        x87_ferr_clear();

    /* The frame it pushed. */
    sp = stack32 ? ESP : SP;
    if (!(cpu_state.CR0.l & 1)) {
        ip  = readmemwl(cpu_state.seg_ss.base + sp);
        rcs = readmemwl(cpu_state.seg_ss.base + sp + 2);
        fl  = readmemwl(cpu_state.seg_ss.base + sp + 4);
        ret = (rcs << 4) + ip;
    } else {
        uint32_t hi;

        if (!hv_lin_readl(&st.sregs, st.sregs.idt.base + 0x84, &hi))
            goto not_mf;
        is32 = !!(hi & 0x800);
        ip  = is32 ? readmemll(cpu_state.seg_ss.base + sp) : readmemwl(cpu_state.seg_ss.base + sp);
        rcs = (is32 ? readmemll(cpu_state.seg_ss.base + sp + 4) : readmemwl(cpu_state.seg_ss.base + sp + 2)) & 0xffff;
        fl  = is32 ? readmemll(cpu_state.seg_ss.base + sp + 8) : readmemwl(cpu_state.seg_ss.base + sp + 4);
        if (fl & 0x20000)
            ret = (rcs << 4) + (ip & 0xffff);
        else {
            uint32_t d0 = readmemll(((rcs & 4) ? ldt.base : gdt.base) + (rcs & ~7));
            uint32_t d1 = readmemll(((rcs & 4) ? ldt.base : gdt.base) + (rcs & ~7) + 4);

            ret = ((d0 >> 16) | ((d1 & 0xff) << 16) | (d1 & 0xff000000)) + ip;
        }
    }
    if (cpu_state.abrt)
        goto not_mf;

    /* A software INT 10h returns past its CD 10. */
    if ((readmembl(ret - 2) == 0xcd) && (readmembl(ret - 1) == 0x10))
        goto not_mf;
    for (n = 0, pc = ret; n < 15; n++, pc++) {
        b = readmembl(pc);
        if ((b != 0x26) && (b != 0x2e) && (b != 0x36) && (b != 0x3e) && (b != 0x64) && (b != 0x65) && (b != 0x66) &&
            (b != 0x67) && (b != 0xf0) && (b != 0xf2) && (b != 0xf3))
            break;
    }
    if (!((b >= 0xd8) && (b <= 0xdf)) && (b != 0x9b) && (b != 0x0f))
        goto not_mf;
    if (b == 0x0f) {
        b = readmembl(pc + 1);
        if (!(((b >= 0x60) && (b <= 0x7f)) || (b >= 0xd0)))
            goto not_mf;
    }
    if (cpu_state.abrt)
        goto not_mf;

    /* Undo the #MF. */
    if (!(cpu_state.CR0.l & 1)) {
        SP = (SP + 6) & 0xffff;
        cpu_state.pc          = ip;
        cpu_state.seg_cs.seg  = rcs;
        cpu_state.seg_cs.base = rcs << 4;
        cpu_state.flags       = (fl & 0xffff) | 2;
    } else {
        pmodeiret(is32);
        if (cpu_state.abrt) {
            cpu_state.abrt = 0;
            hv_state_begin(&st);
            goto not_mf;
        }
    }

    /* The instruction, as the board takes its error: it runs with IGNNE#,
       or FERR# goes up, and it waits for the IRQ 13 handler. */
    pc = cpu_state.pc;
    exec386_hv_step();
    if ((cpu_state.pc == pc) && !(cpu_state.CR0.l & 0x20)) {
        hv_halted    = 2;
        run->if_flag = !!(cpu_state.flags & I_FLAG);
    }
    hv_state_end(&st);
    hv_ferr_snap = st.fpu;
    if (++hv_stat_ferr <= 4)
        pclog("KVM: x87 error at %04X:%08X reported on FERR#%s\n", CS, pc, (cpu_state.pc == pc) ? "" : ", ignored (IGNNE#)");
    return;

not_mf:
    cpu_state.abrt = 0;
    hv_bp_resume();
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

/* Auto HALT restart. An SMI taken in the HALT state returns to it, unless
   the SMM handler clears bit 0 of the state save map's auto HALT restart
   field, which KVM, halting in 86Box rather than in the kernel, neither
   sets nor reads. The guest is stopped at the handler's entry to set it,
   and where RSM returns to, to see whether it was left set. */
#define HV_SMRAM_AUTOHALT 0xff02 /* KVM's 32-bit state save map, Intel's */
#define HV_SMRAM_CS_BASE  0xff98
#define HV_SMRAM_EIP      0xfff0
static uint32_t hv_smm_smbase;

static void
hv_smm_halt_arm(void)
{
    struct {
        struct kvm_msrs      hdr;
        struct kvm_msr_entry e;
    } msrs;

    memset(&msrs, 0, sizeof(msrs));
    msrs.hdr.nmsrs = 1;
    msrs.e.index   = 0x9e; /* MSR_IA32_SMBASE, the host's to read */
    if (ioctl(cpu_fd, KVM_GET_MSRS, &msrs) != 1)
        return;
    hv_smm_smbase = (uint32_t) msrs.e.data;
    hv_bp(HV_BP_SMM, 1, hv_smm_smbase + 0x8000);
}

/* In SMM, at the handler's entry. */
static void
hv_smm_halt_entry(void)
{
    uint32_t ret;

    hv_bp(HV_BP_SMM, 0, 0);
    if (!in_smm)
        return;
    mem_view_write(MEM_VIEW_CPU, hv_smm_smbase + HV_SMRAM_AUTOHALT, 1, 2);
    ret = mem_view_read(MEM_VIEW_CPU, hv_smm_smbase + HV_SMRAM_CS_BASE, 4) +
          mem_view_read(MEM_VIEW_CPU, hv_smm_smbase + HV_SMRAM_EIP, 4);
    hv_bp(HV_BP_SMM_RET, 1, ret);
}

/* Out of SMM, where it returned to, past the HLT. */
static void
hv_smm_halt_return(void)
{
    uint32_t       addr = hv_smm_smbase + HV_SMRAM_AUTOHALT;
    int            ro;
    mem_mapping_t *dirty;
    uint8_t       *host;

    hv_bp(HV_BP_SMM_RET, 0, 0);
    if (in_smm) {
        hv_bp_resume();
        return;
    }
    host = hv_slot_at(1, addr >> MEM_GRANULARITY_BITS, &ro, &dirty);
    if (host && (*(uint16_t *) (host + (addr & MEM_GRANULARITY_MASK)) & 1))
        hv_halted = 1;
}

/* A breakpoint of ours, or the guest's own debug exception. */
static void
hv_debug_exit(void)
{
    uint64_t dr6 = run->debug.arch.dr6;

    if ((run->debug.arch.exception == 1) && (dr6 & 1) && hv_bp_on[HV_BP_FERR])
        hv_ferr_hit();
    else if ((run->debug.arch.exception == 1) && (dr6 & 2) && hv_bp_on[HV_BP_SMM])
        hv_smm_halt_entry();
    else if ((run->debug.arch.exception == 1) && (dr6 & 4) && hv_bp_on[HV_BP_SMM_RET])
        hv_smm_halt_return();
    else
        hv_bp_apply(1);
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
            if (hv_smi_halted)
                hv_smm_halt_arm();
        }
        hv_smi_halted = 0;
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
            /* An SMI ends a HLT for its stay in SMM only (auto HALT restart). */
            hv_smi_halted = smi_line && (hv_halted == 1) && !hv_reset_pending;
            hv_halted     = 0;
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

    /* MSRs that KVM does not know are the emulated processor's, and so are
       a few that it answers with a made up value where the emulated one
       has the board's: EBL_CR_POWERON, the bus ratio straps, which P6 BIOSes
       divide by (a zero hangs the P2B-LS's POST on #DE, and has the 6VIA90AP
       show 221x5.0 MHz), and the platform ID that selects microcode. */
    memset(&cap, 0, sizeof(cap));
    cap.cap     = KVM_CAP_X86_USER_SPACE_MSR;
    cap.args[0] = KVM_MSR_EXIT_REASON_UNKNOWN | KVM_MSR_EXIT_REASON_FILTER;
    if (ioctl(vm_fd, KVM_ENABLE_CAP, &cap) < 0)
        fatal("KVM: ENABLE_CAP X86_USER_SPACE_MSR failed: %s\n", strerror(errno));
    {
        static uint8_t         deny = 0x00;
        struct kvm_msr_filter  filter;
        static const uint32_t  own[] = { 0x00000017, 0x0000002a };

        memset(&filter, 0, sizeof(filter));
        filter.flags = KVM_MSR_FILTER_DEFAULT_ALLOW;
        for (uint32_t i = 0; i < (sizeof(own) / sizeof(own[0])); i++) {
            filter.ranges[i].flags  = KVM_MSR_FILTER_READ;
            filter.ranges[i].nmsrs  = 1;
            filter.ranges[i].base   = own[i];
            filter.ranges[i].bitmap = &deny;
        }
        if (ioctl(vm_fd, KVM_X86_SET_MSR_FILTER, &filter) < 0)
            pclog("KVM: MSR filter not set (%s), the bus ratio MSR reads 0\n", strerror(errno));
    }

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
    hv_pin_ok = 1;

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

/* A run of the guest that ended well past the signal meant to stop it: the
   vCPU stood still for that long inside KVM_RUN, and the guest's clocks all
   jumped by as much (Linux: "hrtimer: interrupt took ... ns"). Said, with
   what the host was doing meanwhile, as far as this call of hv_exec() shows
   it: another thread on the CPU (involuntary switches, little CPU time), a
   page of the guest's memory read back from swap (major faults), or the
   kernel or guest on the CPU all along (CPU time close to the wall time). */
#define HV_STALL_NS 2000000ULL
static uint64_t        hv_call_ns;
static struct timespec hv_call_cpu;
static struct rusage   hv_call_ru;
static uint64_t        hv_stat_stalls;
static uint64_t        hv_stat_stalls_next = 10;

static void
hv_stall_begin(void)
{
    hv_call_ns = hv_ns_last;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &hv_call_cpu);
    getrusage(RUSAGE_THREAD, &hv_call_ru);
}

static void
hv_stall_check(uint64_t deadline)
{
    struct timespec cpu;
    struct rusage   ru;
    int64_t         late = (int64_t) (hv_ns_last - deadline);
    int64_t         cpu_ns;

    if (late < (int64_t) HV_STALL_NS)
        return;

    hv_stat_stalls++;
    if ((hv_stat_stalls > 4) && (hv_stat_stalls != hv_stat_stalls_next))
        return;
    if (hv_stat_stalls >= hv_stat_stalls_next)
        hv_stat_stalls_next *= 10;

    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
    getrusage(RUSAGE_THREAD, &ru);
    cpu_ns = ((int64_t) (cpu.tv_sec - hv_call_cpu.tv_sec) * 1000000000LL) + (cpu.tv_nsec - hv_call_cpu.tv_nsec);
    pclog("KVM: the guest stood still for %.1f ms (%" PRIu64 " so far); this call: %.1f ms, %.1f ms of it on a CPU, "
          "%ld involuntary context switches, %ld major page faults\n",
          late / 1000000.0, hv_stat_stalls, (hv_ns_last - hv_call_ns) / 1000000.0, cpu_ns / 1000000.0,
          ru.ru_nivcsw - hv_call_ru.ru_nivcsw, ru.ru_majflt - hv_call_ru.ru_majflt);
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
    hv_stall_begin();

    /* What the guest wrote into a device's memory since the last call, for
       the device to see before its timers next look. */
    hv_dirty_all();

    /* What the guest changes without stopping, every call: paging (for
       pinned shadow RAM), CR0.NE and its vector 16 handler (FERR#). */
    hv_poll();

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
        hv_bp_apply(0);

        /* Only cycles of devices, not of the processor, are counted now. */
        cycles = 0;

        ret = ioctl(cpu_fd, KVM_RUN, 0);
        hv_account_run(0);
        hv_stall_check(deadline);
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

        /* RSM went somewhere else than it was to return to. */
        if (hv_bp_on[HV_BP_SMM_RET] && !in_smm && (run->exit_reason != KVM_EXIT_DEBUG))
            hv_bp(HV_BP_SMM_RET, 0, 0);
        /* FERR# down as the IRQ 13 handler clears the error. */
        if (fpu_ferr) {
            struct kvm_fpu fpu;

            if ((ioctl(cpu_fd, KVM_GET_FPU, &fpu) == 0) && !(fpu.fsw & 0x80))
                x87_ferr_clear();
        }

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

            case KVM_EXIT_DEBUG:
                hv_debug_exit();
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
                if (run->internal.suberror == KVM_INTERNAL_ERROR_EMULATION) {
                    hv_emulate();
                    break;
                }
                if ((run->internal.suberror == KVM_INTERNAL_ERROR_DELIVERY_EV) && (run->internal.ndata >= 4) &&
                    hv_pin_failed(run->internal.data[3]))
                    break;
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
