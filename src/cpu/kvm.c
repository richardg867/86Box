/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Linux KVM API layer of the hypervisor CPU back-end (the host
 *          independent part, time and the memory map, is hv_common.c).
 *
 *          Time: a POSIX timer aimed at this thread stops the guest when
 *          the next 86Box timer is due. The guest's TSC follows 86Box's
 *          clock through KVM's TSC offset.
 *
 *          Memory: the runs of granules hv_common.c maps straight into the
 *          guest are KVM memory slots, in address space 0 for normal
 *          operation and address space 1 for SMM. Writes to mappings that
 *          may queue them are coalesced MMIO.
 *
 *          SMM is KVM's: SMIs are injected with KVM_SMI, and KVM saves and
 *          restores the state and runs RSM.
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
#include "hv_internal.h"

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

static int             kvm_fd   = -1;
static int             vm_fd    = -1;
static int             cpu_fd   = -1;
static int             run_size = 0;
static struct kvm_run *run      = MAP_FAILED;
static int             hv_avail = -1;

const char *hv_name = "KVM";

static int hv_has_smm;

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

static int       hv_sig;
static sigset_t  hv_sigset;
static timer_t   hv_timer;
static int       hv_timer_valid;
static uint64_t  hv_armed_ns;
static pthread_t hv_thread;

static int      hv_tsc_attr;   /* KVM_VCPU_TSC_OFFSET is there */
static uint64_t hv_tsc_base;   /* the guest TSC's offset from the host's at the start */
static uint64_t hv_tsc_khz;
static int64_t  hv_tsc_applied; /* the lag the offset holds, in TSC ticks */

static int hv_smi_entering;

/* Statistics, logged every few seconds with ENABLE_KVM_LOG. */
static uint64_t hv_stat_exits[64];
static uint64_t hv_stat_intr;
static uint64_t hv_stat_slot_ops;
static uint64_t hv_stat_ferr;

static void
hv_sig_handler(UNUSED(int sig))
{
    /* Only here to make KVM_RUN return. */
}

uint64_t
hv_clock_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t) ts.tv_sec * 1000000000ULL) + ts.tv_nsec;
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

/* hv_common.c's view of it. */
uint8_t *
hv_be_hma_window(void)
{
    return hv_hma_setup() ? hv_hma_win : NULL;
}

void
hv_be_hma_follow(void)
{
    if (hv_hma_win && (hv_hma_ram == ram))
        hv_hma_point(!mem_a20_state);
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

void
hv_be_slot_map(int as, const hv_slot_t *slot)
{
    hv_slot_ioctl(as, slot->id, slot->start, slot->end, slot->host, slot->ro, !!slot->dirty);
}

void
hv_be_slot_unmap(int as, const hv_slot_t *slot)
{
    hv_slot_ioctl(as, slot->id, slot->start, slot->start, slot->host, 0, 0);
}

void
hv_be_dirty_log(int as, const hv_slot_t *slot, uint64_t *bitmap)
{
    struct kvm_dirty_log log = { 0 };

    log.slot         = (as << 16) | slot->id;
    log.dirty_bitmap = bitmap;
    if (ioctl(vm_fd, KVM_GET_DIRTY_LOG, &log) < 0)
        fatal("KVM: GET_DIRTY_LOG failed: %s\n", strerror(errno));
}

int
hv_be_ready(void)
{
    return vm_fd >= 0;
}

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
void
hv_be_zones(uint32_t lo, uint32_t hi)
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

/* KVM's processor state, in the form hv_common.c works on. What a set
   writes is what the get before it read, with that form's registers put
   in: the rest (the upper registers, EFER, the APIC base, the XMM
   registers) stays as KVM has it. hv_state_begin() and hv_state_end() are
   never apart, so the last get is the one to set again. */
static struct kvm_regs  hv_kregs;
static struct kvm_sregs hv_ksregs;
static struct kvm_fpu   hv_kfpu;

static void
hv_seg_from_kvm(hv_seg_t *dest, const struct kvm_segment *src)
{
    dest->base     = src->base;
    dest->limit    = src->limit;
    dest->selector = src->selector;
    dest->type     = src->type;
    dest->present  = src->present;
    dest->dpl      = src->dpl;
    dest->db       = src->db;
    dest->s        = src->s;
    dest->l        = src->l;
    dest->g        = src->g;
    dest->avl      = src->avl;
    dest->unusable = src->unusable;
}

static void
hv_seg_to_kvm(struct kvm_segment *dest, const hv_seg_t *src)
{
    dest->base     = src->base;
    dest->limit    = src->limit;
    dest->selector = src->selector;
    dest->type     = src->type;
    dest->present  = src->present;
    dest->dpl      = src->dpl;
    dest->db       = src->db;
    dest->s        = src->s;
    dest->l        = src->l;
    dest->g        = src->g;
    dest->avl      = src->avl;
    dest->unusable = src->unusable;
}

static void
hv_sregs_from_kvm(hv_sregs_t *dest, const struct kvm_sregs *src)
{
    hv_seg_from_kvm(&dest->cs, &src->cs);
    hv_seg_from_kvm(&dest->ds, &src->ds);
    hv_seg_from_kvm(&dest->es, &src->es);
    hv_seg_from_kvm(&dest->fs, &src->fs);
    hv_seg_from_kvm(&dest->gs, &src->gs);
    hv_seg_from_kvm(&dest->ss, &src->ss);
    hv_seg_from_kvm(&dest->tr, &src->tr);
    hv_seg_from_kvm(&dest->ldt, &src->ldt);
    dest->gdt.base  = src->gdt.base;
    dest->gdt.limit = src->gdt.limit;
    dest->idt.base  = src->idt.base;
    dest->idt.limit = src->idt.limit;
    dest->cr0       = src->cr0;
    dest->cr2       = src->cr2;
    dest->cr3       = src->cr3;
    dest->cr4       = src->cr4;
}

int
hv_be_get_state(hv_regs_t *regs, hv_sregs_t *sregs, hv_fpu_t *fpu)
{
    if (cpu_fd < 0) {
        errno = EBADF;
        return -1;
    }
    if ((regs && (ioctl(cpu_fd, KVM_GET_REGS, &hv_kregs) < 0)) || (sregs && (ioctl(cpu_fd, KVM_GET_SREGS, &hv_ksregs) < 0)) ||
        (fpu && (ioctl(cpu_fd, KVM_GET_FPU, &hv_kfpu) < 0)))
        return -1;

    if (regs) {
        regs->rax    = hv_kregs.rax;
        regs->rbx    = hv_kregs.rbx;
        regs->rcx    = hv_kregs.rcx;
        regs->rdx    = hv_kregs.rdx;
        regs->rsi    = hv_kregs.rsi;
        regs->rdi    = hv_kregs.rdi;
        regs->rsp    = hv_kregs.rsp;
        regs->rbp    = hv_kregs.rbp;
        regs->rip    = hv_kregs.rip;
        regs->rflags = hv_kregs.rflags;
    }
    if (sregs)
        hv_sregs_from_kvm(sregs, &hv_ksregs);
    if (fpu) {
        memcpy(fpu->fpr, hv_kfpu.fpr, sizeof(fpu->fpr));
        fpu->fcw         = hv_kfpu.fcw;
        fpu->fsw         = hv_kfpu.fsw;
        fpu->ftwx        = hv_kfpu.ftwx;
        fpu->last_opcode = hv_kfpu.last_opcode;
        fpu->last_ip     = hv_kfpu.last_ip;
        fpu->last_dp     = hv_kfpu.last_dp;
    }

    return 0;
}

void
hv_be_set_state(const hv_regs_t *regs, const hv_sregs_t *sregs, const hv_fpu_t *fpu)
{
    if (regs) {
        hv_kregs.rax    = regs->rax;
        hv_kregs.rbx    = regs->rbx;
        hv_kregs.rcx    = regs->rcx;
        hv_kregs.rdx    = regs->rdx;
        hv_kregs.rsi    = regs->rsi;
        hv_kregs.rdi    = regs->rdi;
        hv_kregs.rsp    = regs->rsp;
        hv_kregs.rbp    = regs->rbp;
        hv_kregs.rip    = regs->rip;
        hv_kregs.rflags = regs->rflags;
    }
    if (sregs) {
        hv_seg_to_kvm(&hv_ksregs.cs, &sregs->cs);
        hv_seg_to_kvm(&hv_ksregs.ds, &sregs->ds);
        hv_seg_to_kvm(&hv_ksregs.es, &sregs->es);
        hv_seg_to_kvm(&hv_ksregs.fs, &sregs->fs);
        hv_seg_to_kvm(&hv_ksregs.gs, &sregs->gs);
        hv_seg_to_kvm(&hv_ksregs.ss, &sregs->ss);
        hv_seg_to_kvm(&hv_ksregs.tr, &sregs->tr);
        hv_seg_to_kvm(&hv_ksregs.ldt, &sregs->ldt);
        hv_ksregs.gdt.base  = sregs->gdt.base;
        hv_ksregs.gdt.limit = sregs->gdt.limit;
        hv_ksregs.idt.base  = sregs->idt.base;
        hv_ksregs.idt.limit = sregs->idt.limit;
        hv_ksregs.cr0       = sregs->cr0;
        hv_ksregs.cr2       = sregs->cr2;
        hv_ksregs.cr3       = sregs->cr3;
        hv_ksregs.cr4       = sregs->cr4;
    }
    if (fpu) {
        memcpy(hv_kfpu.fpr, fpu->fpr, sizeof(hv_kfpu.fpr));
        hv_kfpu.fcw         = fpu->fcw;
        hv_kfpu.fsw         = fpu->fsw;
        hv_kfpu.ftwx        = fpu->ftwx;
        hv_kfpu.last_opcode = fpu->last_opcode;
        hv_kfpu.last_ip     = fpu->last_ip;
        hv_kfpu.last_dp     = fpu->last_dp;
    }

    if ((regs && (ioctl(cpu_fd, KVM_SET_REGS, &hv_kregs) < 0)) || (sregs && (ioctl(cpu_fd, KVM_SET_SREGS, &hv_ksregs) < 0)) ||
        (fpu && (ioctl(cpu_fd, KVM_SET_FPU, &hv_kfpu) < 0)))
        fatal("KVM: setting the processor state failed: %s\n", strerror(errno));
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
    struct kvm_sregs ksr;
    hv_sregs_t       sr;
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

    if (ioctl(cpu_fd, KVM_GET_SREGS, &ksr) < 0)
        goto done;
    hv_sregs_from_kvm(&sr, &ksr);
    hv_pin_poll(sr.cr0);
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
static hv_fpu_t hv_ferr_snap;

static int
hv_ferr_same(const hv_fpu_t *a, const hv_fpu_t *b)
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
    uint64_t data = run->msr.data;

    run->msr.error = hv_msr_access(run->msr.index, write, &data);
    if (!write)
        run->msr.data = data;
    kvm_log("KVM: %sMSR %08X %016" PRIX64 "%s\n", write ? "WR" : "RD", run->msr.index, (uint64_t) run->msr.data,
            run->msr.error ? " #GP" : "");
}

static void
hv_io(void)
{
    hv_io_access(run->io.port, run->io.size, run->io.direction == KVM_EXIT_IO_OUT, (uint8_t *) run + run->io.data_offset,
                 run->io.count);
}

static void
hv_mmio(void)
{
    hv_mmio_access((uint32_t) run->mmio.phys_addr, run->mmio.data, run->mmio.len, run->mmio.is_write);
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

/* Halted: wait for the timer's signal, or deadline. */
void
hv_be_sleep(uint64_t deadline)
{
    struct timespec ts;

    ts.tv_sec  = (deadline - hv_ns_last) / 1000000000ULL;
    ts.tv_nsec = (deadline - hv_ns_last) % 1000000000ULL;
    if (sigtimedwait(&hv_sigset, NULL, &ts) > 0)
        hv_armed_ns = 0;
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

    /* A memory slot change drops only that slot's mappings, not all of the
       guest's to be faulted back in: the chipset changes slots often. */
#ifdef KVM_X86_QUIRK_SLOT_ZAP_ALL
    if (ioctl(vm_fd, KVM_CHECK_EXTENSION, KVM_CAP_DISABLE_QUIRKS2) & KVM_X86_QUIRK_SLOT_ZAP_ALL) {
        memset(&cap, 0, sizeof(cap));
        cap.cap     = KVM_CAP_DISABLE_QUIRKS2;
        cap.args[0] = KVM_X86_QUIRK_SLOT_ZAP_ALL;
        ioctl(vm_fd, KVM_ENABLE_CAP, &cap);
    }
#endif

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
    hv_mem_init();

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

/* The guest's RAM, allocated by the host up front. Left to the guest's
   first touch, each page is allocated inside KVM_RUN, where finding a huge
   page on a host whose memory is full of cache can mean compacting it, in
   the kernel, for tens of milliseconds the guest's clocks then jump by
   (stalls with CPU time close to the wall time, during POST's memory
   test). */
#ifndef MADV_POPULATE_WRITE
#    define MADV_POPULATE_WRITE 23 /* Linux 5.14; older headers lack it, an older kernel refuses it */
#endif
static uint8_t *hv_populated;

static void
hv_populate(void)
{
    uint64_t t;
    size_t   size;

    if ((ram == NULL) || (ram == hv_populated))
        return;
    hv_populated = ram;
    size         = ((size_t) mem_size << 10) & ~(size_t) 0xfff;
    t            = hv_clock_ns();
    if (madvise(ram, size, MADV_POPULATE_WRITE) < 0)
        pclog("KVM: allocating the guest's RAM up front failed: %s\n", strerror(errno));
    else
        pclog("KVM: allocated %zu MB of guest RAM up front in %.1f ms\n", size >> 20, (hv_clock_ns() - t) / 1000000.0);
}

static uint64_t        hv_call_ns;
static struct timespec hv_call_cpu;
static struct rusage   hv_call_ru;
static uint64_t        hv_stat_stalls;
static uint64_t        hv_stat_stalls_next = 10;
static uint64_t        hv_stall_logged;

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

    /* The first few, every power of ten, and any of 5 ms or more, once a
       second at most. */
    hv_stat_stalls++;
    if ((hv_stat_stalls > 4) && (hv_stat_stalls != hv_stat_stalls_next) &&
        ((late < 5000000LL) || ((int64_t) (hv_ns_last - hv_stall_logged) < 1000000000LL)))
        return;
    if (hv_stat_stalls >= hv_stat_stalls_next)
        hv_stat_stalls_next *= 10;
    hv_stall_logged = hv_ns_last;

    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
    getrusage(RUSAGE_THREAD, &ru);
    cpu_ns = ((int64_t) (cpu.tv_sec - hv_call_cpu.tv_sec) * 1000000000LL) + (cpu.tv_nsec - hv_call_cpu.tv_nsec);
    pclog("KVM: the guest stood still for %.1f ms (%" PRIu64 " so far); this call: %.1f ms, %.1f ms of it on a CPU, "
          "%ld involuntary context switches, %ld major and %ld minor page faults\n",
          late / 1000000.0, hv_stat_stalls, (hv_ns_last - hv_call_ns) / 1000000.0, cpu_ns / 1000000.0,
          ru.ru_nivcsw - hv_call_ru.ru_nivcsw, ru.ru_majflt - hv_call_ru.ru_majflt, ru.ru_minflt - hv_call_ru.ru_minflt);
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
    hv_populate();
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

        if (hv_halted && !hv_halt(end, run->if_flag))
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
