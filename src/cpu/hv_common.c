/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Hypervisor CPU back-ends: the part that does not depend on the
 *          host's hypervisor API (kvm.c, whpx.c).
 *
 *          Time: the TSC that 86Box's timers count in follows the host's
 *          monotonic clock, at the emulated CPU's clock rate. It is
 *          brought up to date whenever the guest stops, before the exit is
 *          handled, so every device sees the time at which the guest
 *          accessed it.
 *
 *          Memory: the CPU's view of the memory map (_mem_exec[],
 *          read_mapping[], write_mapping[]) is translated into runs of
 *          memory mapped straight into the guest (hv_slot_t), in address
 *          space 0 for normal operation and address space 1 for SMM where
 *          the hypervisor has one. Memory a mapping reads straight from
 *          its exec memory is mapped, read only unless its writes go there
 *          too; everything else stays an exit handled by 86Box.
 *
 *
 *
 * Authors: RichardG, <richardg867@gmail.com>
 *
 *          Copyright 2026 RichardG.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <errno.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include "x86.h"
#include "x86_interrupt.h"
#include "x87_sf.h"
#include "x87.h"
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/nmi.h>
#include <86box/pic.h>
#include <86box/smram.h>
#include <86box/timer.h>
#include <86box/hypervisor.h>
#include <86box/plat_unused.h>
/* hv_sregs_t has members of these names. */
#undef cs
#undef ds
#undef es
#undef ss
#undef gs
#undef cr0
#include "hv_internal.h"

#ifdef ENABLE_HV_LOG
int hv_do_log = ENABLE_HV_LOG;

static void
hv_log(const char *fmt, ...)
{
    va_list ap;

    if (hv_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define hv_log(fmt, ...)
#endif

int hv_active = 0;
int hv_rsm    = 0;

int       hv_as_count; /* 2 with the hypervisor's SMM */
hv_slot_t hv_slots[2][HV_MAX_SLOTS];
int       hv_nslots[2];
static uint8_t   hv_slot_id_used[2][HV_MAX_SLOTS];
static hv_slot_t hv_runs[HV_MAX_SLOTS];
/* How often each 16 KB below 1 MB went between read only and writable. */
uint8_t hv_ro_flips[2][0x100000 >> 14];
int     hv_pin_ok;
int     hv_in_recalc;
int     hv_npending;
int     hv_pending_all;

uint64_t hv_min_run_ns = HV_MIN_RUN_NS;
uint64_t hv_ns_last;    /* host clock at the last accounting */
uint64_t hv_lag_ns;     /* host time held back from the guest */
uint64_t hv_behind_ns;  /* the guest's time behind the host's since the start */
uint64_t hv_tsc_frac;
uint64_t hv_tsc_per_ns; /* 32.32 */
double   hv_cpuclock;

int hv_halted;
int hv_reset_pending;
int hv_smi_halted; /* the SMI ended a HLT */

uint64_t hv_stat_emulated;
uint64_t hv_stat_emulated_next = 10;

/* Time. */
void
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
void
hv_advance(uint64_t ns)
{
    uint128_t t = ((uint128_t) ns * hv_tsc_per_ns) + hv_tsc_frac;

    tsc        += (uint64_t) (t >> 32);
    hv_tsc_frac = (uint32_t) t;
}

/* Host time that passed while the guest ran or was halted: all of it, and
   some of the lag on top. */
void
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
void
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
        hv_log("%s: dropping %" PRIu64 " ms of lag\n", hv_name, (hv_lag_ns - HV_MAX_LAG_NS) / 1000000ULL);
        hv_lag_ns = HV_MAX_LAG_NS;
    }
}

/* A timer's due time on the host's clock. */
uint64_t
hv_ts_to_host(uint64_t ts)
{
    int64_t rem = (int64_t) (ts - tsc);

    if (rem <= 0)
        return hv_ns_last;

    return hv_ns_last + (uint64_t) (((uint128_t) rem << 32) / hv_tsc_per_ns) + 1;
}

/* When the next timer is due, on the host's clock. */
uint64_t
hv_timer_deadline(void)
{
    if (!timer_head)
        return hv_ns_last + 1000000000ULL;

    return hv_ts_to_host(timer_target);
}

/* When the guest is to be stopped for its timers: the next one, but no
   sooner than hv_min_run_ns from now unless it is a precise one (see
   TIMER_PRECISE), which is never run late. */
uint64_t
hv_stop_deadline(void)
{
    uint64_t         deadline = hv_timer_deadline();
    const pc_timer_t *t;

    if ((int64_t) (deadline - (hv_ns_last + hv_min_run_ns)) >= 0)
        return deadline;

    for (t = timer_head; t != NULL; t = t->next) {
        if (t->flags & TIMER_PRECISE)
            return MIN(hv_ts_to_host(t->ts_integer), hv_ns_last + hv_min_run_ns);
        if ((int64_t) (hv_ts_to_host(t->ts_integer) - (hv_ns_last + hv_min_run_ns)) >= 0)
            break;
    }

    return hv_ns_last + hv_min_run_ns;
}

/* Memory slots. */
static uint8_t *
hv_resolve(int as, uint32_t g, int *ro, mem_mapping_t **dirty)
{
    int      a20 = mem_a20_state || (as && unmask_a20_in_smm);
    int      writable;
    uint8_t *ret;
    uint8_t *win;

    /* With A20 masked, the high memory area is the first 64 KB again. Only
       that, which real mode code can reach, is followed: the rest of memory
       would be thousands of slot changes at every toggle. */
    if ((g >= (HV_HMA >> MEM_GRANULARITY_BITS)) && (g < ((HV_HMA + HV_HMA_SIZE) >> MEM_GRANULARITY_BITS))) {
        uint32_t lo = g - (HV_HMA >> MEM_GRANULARITY_BITS);
        int      lo_writable;
        uint8_t *lo_ret = mem_granule_direct(lo, &lo_writable, dirty);

        ret = mem_granule_direct(g, &writable, dirty);
        /* Both RAM, as they are: the window, whichever it shows. */
        if (!(as && unmask_a20_in_smm) && ((win = hv_be_hma_window()) != NULL) && ret && lo_ret && writable && lo_writable && !*dirty &&
            (ret == (ram + (g << MEM_GRANULARITY_BITS))) && (lo_ret == (ram + (lo << MEM_GRANULARITY_BITS)))) {
            *ro = 0;
            return win + (lo << MEM_GRANULARITY_BITS);
        }
        if (!a20)
            g = lo;
    }

    ret = mem_granule_direct(g, &writable, dirty);
    *ro = !writable;
    return ret;
}

/* Report the pages the guest wrote in a logged slot to their mapping. */
void
hv_dirty_slot(int as, hv_slot_t *slot)
{
    static uint64_t bitmap[(HV_GRANULES / 64) + 1];
    uint32_t        pages = slot->end - slot->start;
    mem_mapping_t  *map   = slot->dirty;

    hv_be_dirty_log(as, slot, bitmap);

    for (uint32_t w = 0; w < ((pages + 63) / 64); w++) {
        uint64_t bits = bitmap[w];

        while (bits) {
            int b = __builtin_ctzll(bits);

            bits &= bits - 1;
            map->direct_dirty(slot->host + ((size_t) (w * 64 + b) << MEM_GRANULARITY_BITS), map->direct_priv);
        }
    }
}

void
hv_dirty_all(void)
{
    for (int as = 0; as < hv_as_count; as++) {
        for (int i = 0; i < hv_nslots[as]; i++) {
            if (hv_slots[as][i].dirty)
                hv_dirty_slot(as, &hv_slots[as][i]);
        }
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
uint8_t *
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
   change makes the hypervisor wait for every reader of the old ones. */
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
               over memory: the hypervisor cannot push an exception frame,
               switch tasks or run every instruction on memory that is not
               mapped writable, and an operating system may use that RAM for
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
                fatal("%s: too many memory slots\n", hv_name);
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
        fatal("%s: too many memory slots\n", hv_name);

    for (i = first; i < last; i++) {
        /* What was written to a logged slot since the last look. */
        if (slots[i].dirty)
            hv_dirty_slot(as, &slots[i]);
        hv_be_slot_unmap(as, &slots[i]);
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
        hv_be_slot_map(as, &slots[first + i]);
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
                hv_be_zones((uint32_t) ((base + o) >> MEM_GRANULARITY_BITS),
                            (uint32_t) MIN((end + o + MEM_GRANULARITY_MASK) >> MEM_GRANULARITY_BITS, HV_GRANULES));
        }
    } else {
        hv_apply_range(as, (uint32_t) (base >> MEM_GRANULARITY_BITS),
                       (uint32_t) ((end + MEM_GRANULARITY_MASK) >> MEM_GRANULARITY_BITS));
        if (!as)
            hv_be_zones((uint32_t) (base >> MEM_GRANULARITY_BITS),
                        (uint32_t) MIN((end + MEM_GRANULARITY_MASK) >> MEM_GRANULARITY_BITS, HV_GRANULES));
    }

    /* The high memory area follows the first 64 KB while A20 is masked. */
    if ((base < 0x10000) && !mem_a20_state)
        hv_apply_range(as, 0x100000 >> MEM_GRANULARITY_BITS, 0x110000 >> MEM_GRANULARITY_BITS);
}

/* A new virtual machine: no slots, the whole memory map to be applied. */
void
hv_mem_init(void)
{
    memset(hv_nslots, 0, sizeof(hv_nslots));
    memset(hv_slot_id_used, 0, sizeof(hv_slot_id_used));
    hv_npending    = 0;
    hv_pending_all = 1;
    memset(hv_ro_flips, 0, sizeof(hv_ro_flips));
    hv_pin_ok = 1;
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

void
hv_mem_changed(uint64_t base, uint64_t size, uint32_t base_ignore)
{
    int i;

    if (!hv_be_ready() || hv_in_recalc || hv_pending_all || !size)
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

void
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
    if (!hv_be_ready())
        return;

    /* The window is pointed at what A20 makes the area; if it is not in use
       the slots there change. */
    hv_be_hma_follow();
    mem_mapping_recalc(HV_HMA, HV_HMA_SIZE, 0);
}

/* State. */
void
hv_cpu_reset(int hard)
{
    /* Applied before the guest runs again: this comes from a port or a timer
       in the middle of an exit. */
    if (hard || (hv_reset_pending < 1))
        hv_reset_pending = hard ? 2 : 1;
}

/* A segment register as the hypervisor holds it, into the CPU emulation's
   form: the bounds an access is checked against, and a null selector's
   mark. */
static void
hv_get_segment(x86seg *dest, const hv_seg_t *src, int pm)
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
hv_put_segment(hv_seg_t *dest, const x86seg *src, int pm)
{
    memset(dest, 0, sizeof(hv_seg_t));
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
   with it, from the hypervisor's. */
void
hv_load_state(const hv_regs_t *regs, const hv_sregs_t *sregs)
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
    hv_regs_t  regs;
    hv_sregs_t sregs;

    if (hv_be_get_state(&regs, &sregs, NULL) < 0)
        return;

    hv_load_state(&regs, &sregs);
}

static x86seg *const hv_state_segs[8] = { &cpu_state.seg_cs, &cpu_state.seg_ds, &cpu_state.seg_es, &cpu_state.seg_ss,
                                          &cpu_state.seg_fs, &cpu_state.seg_gs, &tr, &ldt };

void
hv_state_begin(hv_state_t *st)
{
    if (hv_be_get_state(&st->regs, &st->sregs, &st->fpu) < 0)
        fatal("%s: getting the processor state failed: %s\n", hv_name, strerror(errno));

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

    /* The softfloat FPU's registers are the hypervisor's as they are. */
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

void
hv_state_end(hv_state_t *st)
{
    hv_seg_t *hsegs[8] = { &st->sregs.cs, &st->sregs.ds, &st->sregs.es, &st->sregs.ss,
                           &st->sregs.fs, &st->sregs.gs, &st->sregs.tr, &st->sregs.ldt };
    int       pm       = cpu_state.CR0.l & 1;
    int       changed  = 0;

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
            hv_put_segment(hsegs[i], hv_state_segs[i], pm && !(cpu_state.eflags & VM_FLAG) && (i < 6));
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

    hv_be_set_state(&st->regs, changed ? &st->sregs : NULL, &st->fpu);
}

/* An instruction the hypervisor could not emulate, an MMX or x87 one whose
   memory operand is a device's, run by the CPU interpreter on the
   hypervisor's state. */
void
hv_emulate(void)
{
    hv_state_t st;

    hv_state_begin(&st);
    /* The first few, and how many there have been every power of ten. */
    hv_stat_emulated++;
    if ((hv_stat_emulated <= 4) || (hv_stat_emulated == hv_stat_emulated_next)) {
        pclog("%s: interpreting the instruction at %04X:%08X, which %s could not emulate (%" PRIu64 " so far)\n",
              hv_name, CS, cpu_state.pc, hv_name, hv_stat_emulated);
        if (hv_stat_emulated >= hv_stat_emulated_next)
            hv_stat_emulated_next *= 10;
    }
    exec386_hv_step();
    hv_state_end(&st);
}

/* Guest memory by linear address, through its page tables as the hypervisor
   has them (no PAE), the CPU's view of the memory map. Returns 0 if not
   present. */
int
hv_lin_readl(const hv_sregs_t *sr, uint32_t lin, uint32_t *val)
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

/* Pinned shadow RAM goes back to what the chipset makes it as the guest
   turns paging on. */
void
hv_pin_poll(uint64_t cr0)
{
    int ok = !(cr0 & 0x80000000);

    if (ok == hv_pin_ok)
        return;
    hv_pin_ok = ok;
    if (!ok)
        hv_mem_changed(0, 0x100000, 0);
}

/* The hypervisor could not deliver an event (an exception's frame, a task
   switch) into memory at gpa. Returns 1 if it was pinned shadow RAM, which
   is mapped as it is from now on and the event delivered again. */
int
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

void
hv_dump(const char *why)
{
    hv_sync_regs();
    pclog("%s: %s at %04X:%08X (CR0 %08X)\n", hv_name, why, CS, cpu_state.pc, cpu_state.CR0.l);
    pclog("%s: EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X EBP=%08X ESP=%08X FLAGS=%04X%04X\n",
          hv_name, EAX, EBX, ECX, EDX, ESI, EDI, EBP, ESP, cpu_state.eflags, cpu_state.flags);
    pclog("%s: CS=%04X/%08X DS=%04X/%08X ES=%04X/%08X SS=%04X/%08X in_smm=%d\n",
          hv_name, CS, cpu_state.seg_cs.base, DS, cpu_state.seg_ds.base, ES, cpu_state.seg_es.base,
          SS, cpu_state.seg_ss.base, in_smm);
}

/* SMM, as the bus sees it. */
void
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

/* MSRs that the hypervisor does not know: the emulated processor's. Returns
   1 if the access raises #GP. */
int
hv_msr_access(uint32_t index, int write, uint64_t *data)
{
    uint32_t old_eax = EAX;
    uint32_t old_ecx = ECX;
    uint32_t old_edx = EDX;
    uint32_t old_cr0 = cpu_state.CR0.l;
    int      old_abrt;
    int      error;

    old_abrt       = cpu_state.abrt;
    cpu_state.abrt = 0;
    cpu_state.CR0.l &= ~1; /* CPL 0, which the hypervisor checked */
    ECX            = index;
    if (write) {
        EAX = (uint32_t) *data;
        EDX = (uint32_t) (*data >> 32);
        cpu_WRMSR();
    } else {
        cpu_RDMSR();
        *data = EAX | ((uint64_t) EDX << 32);
    }
    error = !!cpu_state.abrt;

    cpu_state.abrt = old_abrt;
    cpu_state.CR0.l = old_cr0;
    EAX            = old_eax;
    ECX            = old_ecx;
    EDX            = old_edx;

    return error;
}

void
hv_io_access(uint16_t port, int size, int out, uint8_t *data, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++, data += size) {
        if (out) {
            switch (size) {
                case 1:
                    outb(port, *data);
                    break;
                case 2:
                    outw(port, *(uint16_t *) data);
                    break;
                default:
                    outl(port, *(uint32_t *) data);
                    break;
            }
        } else {
            switch (size) {
                case 1:
                    *data = inb(port);
                    break;
                case 2:
                    *(uint16_t *) data = inw(port);
                    break;
                default:
                    *(uint32_t *) data = inl(port);
                    break;
            }
        }
    }
}

/* The access as the processor makes it: the CPU's view of the memory map,
   which is not the bus masters' (mem_readb_phys()) where the chipset or a
   mapping tells them apart (SMRAM, shadow RAM write routing, CPU-only
   mappings). */
void
hv_mmio_access(uint32_t addr, uint8_t *data, uint32_t len, int write)
{
    uint32_t i;

    if (write) {
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

/* Wait out a HLT, until something wakes the processor or the call is up.
   Returns 0 if still halted. */
int
hv_halt(uint64_t end, int if_flag)
{
    uint64_t deadline;
    uint64_t step;
    int64_t  rem;

    while (1) {
        if (hv_reset_pending || smi_line || hv_nmi_pending() || (if_flag && x86_external_interrupt_pending())) {
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
        if ((int64_t) (deadline - hv_ns_last) > 0)
            hv_be_sleep(deadline);

        hv_account_run(1);
        hv_timers();
    }
}
