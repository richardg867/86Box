/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Windows Hypervisor Platform (WHPX) API layer of the hypervisor
 *          CPU back-end (the host independent part, time and the memory
 *          map, is hv_common.c).
 *
 *          Time: a thread of its own cancels WHvRunVirtualProcessor() when
 *          the next 86Box timer is due, on a high resolution waitable
 *          timer, which can wake hundreds of microseconds late: the last
 *          stretch before a precise timer (TIMER_PRECISE) is waited out by
 *          spinning, as long as the timer was seen to be late. The guest's
 *          TSC is kept on 86Box's clock by writing it, WHPX having no TSC
 *          offset that can be set.
 *
 *          Memory: the runs of granules hv_common.c maps straight into the
 *          guest are GPA ranges; everything else is unmapped, and accesses
 *          to it exit. The instructions are carried out by WHPX's
 *          instruction emulator (WinHvEmulation.dll), or by the CPU
 *          interpreter (exec386_hv_step()) where it fails. There is a
 *          single address space: SMRAM is mapped while in SMM. The high
 *          memory area follows A20 by mapping changes.
 *
 *          SMM is 86Box's: WHPX has none. An SMI runs enter_smm() on the
 *          processor's state; RSM, which raises #UD outside of SMM as VMX
 *          has it, is caught while in SMM, and leave_smm() run.
 *
 *          FERR#: with CR0.NE clear, an x87 error is reported on FERR#,
 *          which the board turns into IRQ 13; VMX raises #MF instead.
 *          #MF is caught before it is delivered, and the instruction run
 *          by the CPU interpreter, which reports the error as the board
 *          does.
 *
 *          The DLLs are loaded at run time: without the Windows feature
 *          they are not there, and the CPU is emulated.
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
#include <windows.h>
#include <winhvplatform.h>
#include <winhvemulation.h>
#include <x86intrin.h>
#include <cpuid.h>
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
#include <86box/plat_dynld.h>
#include <86box/plat_unused.h>
/* hv_sregs_t has members of these names. */
#undef cs
#undef ds
#undef es
#undef ss
#undef gs
#undef cr0
#include "hv_internal.h"

#ifdef ENABLE_WHPX_LOG
int whpx_do_log = ENABLE_WHPX_LOG;

static void
whpx_log(const char *fmt, ...)
{
    va_list ap;

    if (whpx_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define whpx_log(fmt, ...)
#endif

const char *hv_name = "WHPX";

/* The API, from the DLLs. */
#define WHPX_FN(f) static __typeof__(f) *p##f
WHPX_FN(WHvGetCapability);
WHPX_FN(WHvCreatePartition);
WHPX_FN(WHvSetupPartition);
WHPX_FN(WHvDeletePartition);
WHPX_FN(WHvSetPartitionProperty);
WHPX_FN(WHvMapGpaRange);
WHPX_FN(WHvUnmapGpaRange);
WHPX_FN(WHvTranslateGva);
WHPX_FN(WHvCreateVirtualProcessor);
WHPX_FN(WHvDeleteVirtualProcessor);
WHPX_FN(WHvRunVirtualProcessor);
WHPX_FN(WHvCancelRunVirtualProcessor);
WHPX_FN(WHvGetVirtualProcessorRegisters);
WHPX_FN(WHvSetVirtualProcessorRegisters);
WHPX_FN(WHvQueryGpaRangeDirtyBitmap);
WHPX_FN(WHvEmulatorCreateEmulator);
WHPX_FN(WHvEmulatorDestroyEmulator);
WHPX_FN(WHvEmulatorTryIoEmulation);
WHPX_FN(WHvEmulatorTryMmioEmulation);

static dllimp_t whpx_imports[] = {
    { "WHvGetCapability",                &pWHvGetCapability                },
    { "WHvCreatePartition",              &pWHvCreatePartition              },
    { "WHvSetupPartition",               &pWHvSetupPartition               },
    { "WHvDeletePartition",              &pWHvDeletePartition              },
    { "WHvSetPartitionProperty",         &pWHvSetPartitionProperty         },
    { "WHvMapGpaRange",                  &pWHvMapGpaRange                  },
    { "WHvUnmapGpaRange",                &pWHvUnmapGpaRange                },
    { "WHvTranslateGva",                 &pWHvTranslateGva                 },
    { "WHvCreateVirtualProcessor",       &pWHvCreateVirtualProcessor       },
    { "WHvDeleteVirtualProcessor",       &pWHvDeleteVirtualProcessor       },
    { "WHvRunVirtualProcessor",          &pWHvRunVirtualProcessor          },
    { "WHvCancelRunVirtualProcessor",    &pWHvCancelRunVirtualProcessor    },
    { "WHvGetVirtualProcessorRegisters", &pWHvGetVirtualProcessorRegisters },
    { "WHvSetVirtualProcessorRegisters", &pWHvSetVirtualProcessorRegisters },
    { "WHvQueryGpaRangeDirtyBitmap",     &pWHvQueryGpaRangeDirtyBitmap     },
    { NULL,                              NULL                              }
};

static dllimp_t whpx_emu_imports[] = {
    { "WHvEmulatorCreateEmulator",   &pWHvEmulatorCreateEmulator   },
    { "WHvEmulatorDestroyEmulator",  &pWHvEmulatorDestroyEmulator  },
    { "WHvEmulatorTryIoEmulation",   &pWHvEmulatorTryIoEmulation   },
    { "WHvEmulatorTryMmioEmulation", &pWHvEmulatorTryMmioEmulation },
    { NULL,                          NULL                          }
};

static void *whpx_dll;
static void *whpx_emu_dll;
static int   hv_avail = -1;

static WHV_PARTITION_HANDLE    whpx_part;
static WHV_EMULATOR_HANDLE     whpx_emu;
static int                     whpx_vp;
static WHV_RUN_VP_EXIT_CONTEXT whpx_exit;

/* The guest's interrupt state as the last exit left it, kept up to date
   with what is set since. */
static uint64_t whpx_rflags;
static int      whpx_shadow;
static int      whpx_ev_pending; /* an event is being or about to be delivered */
static uint64_t whpx_notify;     /* WHvX64RegisterDeliverabilityNotifications as set */
static uint64_t whpx_exc_bitmap; /* the exception exit bitmap as set */
static int      whpx_exc_fixed;  /* it cannot be changed after setup */
static int      whpx_fxsr_only;  /* the emulated processor has FXSR but no SSE */

/* The host's clock. */
static uint64_t whpx_qpc_freq;

/* The guest's TSC: WHPX has no offset to set, only the TSC itself. */
static uint64_t whpx_tsc_khz0;    /* from a short calibration at the start */
static uint64_t whpx_tsc_host0;   /* host TSC and clock at the start, for a longer one */
static uint64_t whpx_tsc_ns0;
static int64_t  whpx_tsc_applied; /* the lag the guest's TSC holds, in TSC ticks */
static uint64_t whpx_tsc_cost;    /* host TSC ticks a register write takes */

/* The thread that stops the guest. */
static HANDLE            whpx_thread;
static HANDLE            whpx_wake;      /* the deadline changed */
static HANDLE            whpx_hrt;       /* its timer */
static HANDLE            whpx_hrt_main;  /* this thread's, for HLT */
static volatile LONG64   whpx_deadline;  /* on the host's clock, 0 for none */
static volatile LONG     whpx_precise;   /* a precise timer is due by then */
static volatile LONG     whpx_quit;
static volatile LONG64   whpx_slack_ns;  /* how late the timer wakes, learned */

/* The x87 as FERR# last went up (see hv_ferr_same() in kvm.c). */
static hv_fpu_t whpx_ferr_snap;

/* Statistics. */
static uint64_t whpx_stat_exits[16];
static uint64_t whpx_stat_emu_fail;
static uint64_t whpx_stat_ferr;
static uint64_t whpx_stat_fxrstor;
static uint64_t whpx_stat_smi;
static uint64_t whpx_stat_late;
static uint64_t whpx_stat_late_next = 10;
static uint64_t whpx_stat_tsc;
static uint64_t whpx_stat_irq;      /* interrupts injected */
static uint64_t whpx_stat_irq_wait; /* pending, the guest not ready */

#define WHPX_STALL_NS 2000000ULL

/* What handling an exit takes, learned: the guest runs for at least
   WHPX_RUN_FACTOR times that before a timer that is not precise stops it,
   which then costs it no more than about an eighth of its time. */
#define WHPX_RUN_FACTOR 8
#define WHPX_MAX_RUN_NS 2000000ULL
static uint64_t whpx_out_ns;
/* How late after its deadline a run stopped by the timer thread ends. */
static uint64_t whpx_late_ns;
#define WHPX_MAX_EARLY_NS 500000ULL
static uint64_t whpx_stat_early;

static void
whpx_fatal(const char *what, HRESULT hr)
{
    fatal("WHPX: %s failed: %08lX\n", what, (unsigned long) hr);
}

/* Time. */
uint64_t
hv_clock_ns(void)
{
    LARGE_INTEGER c;

    QueryPerformanceCounter(&c);
    return (uint64_t) (((uint128_t) (uint64_t) c.QuadPart * 1000000000ULL) / whpx_qpc_freq);
}

/* Whether a precise timer (TIMER_PRECISE) is due by deadline, on the host's
   clock: those are worth waiting for more carefully. */
static int
whpx_deadline_precise(uint64_t deadline)
{
    const pc_timer_t *t;

    for (t = timer_head; t != NULL; t = t->next) {
        if ((int64_t) (hv_ts_to_host(t->ts_integer) - deadline) > 0)
            break;
        if (t->flags & TIMER_PRECISE)
            return 1;
    }

    return 0;
}

/* Wait on a high resolution timer until deadline; for a precise one, the
   timer is set early by as late as it was seen to wake, and the rest spun
   away. Returns 1 if woken by wake (the deadline changed) instead. */
static int
whpx_wait_until(HANDLE timer, uint64_t deadline, int precise, HANDLE wake, volatile LONG64 *watch)
{
    HANDLE        objs[2] = { wake, timer };
    LARGE_INTEGER due;
    uint64_t      now;
    uint64_t      margin;
    uint64_t      target;
    int64_t       late;
    DWORD         r;

    while (1) {
        now = hv_clock_ns();
        if ((int64_t) (deadline - now) <= 0)
            return 0;

        margin = precise ? ((uint64_t) whpx_slack_ns + 20000ULL) : 0;
        if ((deadline - now) > margin) {
            target       = deadline - margin;
            due.QuadPart = -(LONGLONG) ((target - now) / 100ULL);
            if (!due.QuadPart)
                due.QuadPart = -1;
            SetWaitableTimerEx(timer, &due, 0, NULL, NULL, NULL, 0);
            if (wake)
                r = WaitForMultipleObjects(2, objs, FALSE, INFINITE);
            else
                r = WaitForSingleObject(timer, INFINITE);
            if (wake && (r == WAIT_OBJECT_0)) {
                CancelWaitableTimer(timer);
                return 1;
            }

            /* How late it woke: what to wake early by next time. */
            late = (int64_t) (hv_clock_ns() - target);
            if (late < 0)
                late = 0;
            if (late > 2000000)
                late = 2000000;
            whpx_slack_ns += (late - whpx_slack_ns) / 8;
            continue;
        }

        while ((int64_t) (deadline - hv_clock_ns()) > 0) {
            if (watch && (*watch != (LONG64) deadline))
                return 1;
            YieldProcessor();
        }
        return 0;
    }
}

static DWORD WINAPI
whpx_timer_thread(UNUSED(void *param))
{
    uint64_t d;

    while (!whpx_quit) {
        d = (uint64_t) whpx_deadline;
        if (!d) {
            WaitForSingleObject(whpx_wake, INFINITE);
            continue;
        }
        if (whpx_wait_until(whpx_hrt, d, whpx_precise, whpx_wake, &whpx_deadline))
            continue;
        /* Claimed here, or by the guest's thread, which no longer needs it. */
        if (InterlockedCompareExchange64(&whpx_deadline, 0, (LONG64) d) == (LONG64) d)
            pWHvCancelRunVirtualProcessor(whpx_part, 0, 0);
    }

    return 0;
}

/* Have the thread stop the guest at deadline. One that is to go off earlier
   is left alone: going off early costs an exit, nothing else. A stop that
   comes while the guest is not running stops its next run at once. */
static void
whpx_arm(uint64_t deadline)
{
    uint64_t cur = (uint64_t) whpx_deadline;

    if (cur && ((int64_t) (cur - hv_ns_last) > 0) && ((int64_t) (cur - deadline) <= 0))
        return;

    whpx_precise  = whpx_deadline_precise(deadline);
    whpx_deadline = (LONG64) deadline;
    SetEvent(whpx_wake);
}

/* A deadline that passed while the guest was stopped for something else is
   not to stop its next run. */
static void
whpx_disarm_past(void)
{
    uint64_t cur = (uint64_t) whpx_deadline;

    if (cur && ((int64_t) (cur - hv_ns_last) <= 0))
        InterlockedCompareExchange64(&whpx_deadline, 0, (LONG64) cur);
}

void
hv_be_sleep(uint64_t deadline)
{
    uint64_t cur = (uint64_t) whpx_deadline;

    /* The thread's stop would only cut the next run short. */
    if (cur)
        InterlockedCompareExchange64(&whpx_deadline, 0, (LONG64) cur);

    whpx_wait_until(whpx_hrt_main, deadline, whpx_deadline_precise(deadline), NULL, NULL);
}

/* Registers. */
static void
whpx_get(const WHV_REGISTER_NAME *names, uint32_t n, WHV_REGISTER_VALUE *vals)
{
    HRESULT hr = pWHvGetVirtualProcessorRegisters(whpx_part, 0, names, n, vals);

    if (FAILED(hr))
        whpx_fatal("getting registers", hr);
}

static void
whpx_set(const WHV_REGISTER_NAME *names, uint32_t n, const WHV_REGISTER_VALUE *vals)
{
    HRESULT hr = pWHvSetVirtualProcessorRegisters(whpx_part, 0, names, n, vals);

    if (FAILED(hr))
        whpx_fatal("setting registers", hr);
}

static uint64_t
whpx_get1(WHV_REGISTER_NAME name)
{
    WHV_REGISTER_VALUE v;

    whpx_get(&name, 1, &v);
    return v.Reg64;
}

static void
whpx_set1(WHV_REGISTER_NAME name, uint64_t val)
{
    WHV_REGISTER_VALUE v;

    memset(&v, 0, sizeof(v));
    v.Reg64 = val;
    whpx_set(&name, 1, &v);
}

/* The registers hv_common.c works on: the general ones, the system ones,
   the x87's. */
#define WHPX_NREGS  10
#define WHPX_NSREGS 14
#define WHPX_NFPU   10
static const WHV_REGISTER_NAME whpx_state_names[WHPX_NREGS + WHPX_NSREGS + WHPX_NFPU] = {
    WHvX64RegisterRax, WHvX64RegisterRbx, WHvX64RegisterRcx, WHvX64RegisterRdx, WHvX64RegisterRsi,
    WHvX64RegisterRdi, WHvX64RegisterRsp, WHvX64RegisterRbp, WHvX64RegisterRip, WHvX64RegisterRflags,

    WHvX64RegisterCs, WHvX64RegisterDs, WHvX64RegisterEs, WHvX64RegisterFs, WHvX64RegisterGs,
    WHvX64RegisterSs, WHvX64RegisterTr, WHvX64RegisterLdtr, WHvX64RegisterGdtr, WHvX64RegisterIdtr,
    WHvX64RegisterCr0, WHvX64RegisterCr2, WHvX64RegisterCr3, WHvX64RegisterCr4,

    WHvX64RegisterFpMmx0, WHvX64RegisterFpMmx1, WHvX64RegisterFpMmx2, WHvX64RegisterFpMmx3,
    WHvX64RegisterFpMmx4, WHvX64RegisterFpMmx5, WHvX64RegisterFpMmx6, WHvX64RegisterFpMmx7,
    WHvX64RegisterFpControlStatus, WHvX64RegisterXmmControlStatus
};
/* What a set writes is what the get before it read, with hv_common.c's
   registers put in (MXCSR stays as it was). */
static WHV_REGISTER_VALUE whpx_state_vals[WHPX_NREGS + WHPX_NSREGS + WHPX_NFPU];

static void
whpx_seg_from(hv_seg_t *dest, const WHV_X64_SEGMENT_REGISTER *src)
{
    memset(dest, 0, sizeof(hv_seg_t));
    dest->base     = src->Base;
    dest->limit    = src->Limit;
    dest->selector = src->Selector;
    dest->type     = src->SegmentType;
    dest->s        = src->NonSystemSegment;
    dest->dpl      = src->DescriptorPrivilegeLevel;
    dest->present  = src->Present;
    dest->avl      = src->Available;
    dest->l        = src->Long;
    dest->db       = src->Default;
    dest->g        = src->Granularity;
    /* VMX's unusable segment, as WHPX shows it. */
    dest->unusable = !src->Present;
}

static void
whpx_seg_to(WHV_X64_SEGMENT_REGISTER *dest, const hv_seg_t *src)
{
    memset(dest, 0, sizeof(WHV_X64_SEGMENT_REGISTER));
    dest->Base                     = src->base;
    dest->Limit                    = src->limit;
    dest->Selector                 = src->selector;
    dest->SegmentType              = src->type;
    dest->NonSystemSegment         = src->s;
    dest->DescriptorPrivilegeLevel = src->dpl;
    dest->Present                  = src->present && !src->unusable;
    dest->Available                = src->avl;
    dest->Long                     = src->l;
    dest->Default                  = src->db;
    dest->Granularity              = src->g;
}

int
hv_be_get_state(hv_regs_t *regs, hv_sregs_t *sregs, hv_fpu_t *fpu)
{
    WHV_REGISTER_VALUE *v = whpx_state_vals;
    hv_seg_t           *segs[8];

    if (!whpx_part)
        return -1;

    if (regs && sregs)
        whpx_get(whpx_state_names, WHPX_NREGS + WHPX_NSREGS + (fpu ? WHPX_NFPU : 0), v);
    else {
        if (regs)
            whpx_get(whpx_state_names, WHPX_NREGS, v);
        if (sregs)
            whpx_get(&whpx_state_names[WHPX_NREGS], WHPX_NSREGS, &v[WHPX_NREGS]);
    }
    if (fpu && !(regs && sregs))
        whpx_get(&whpx_state_names[WHPX_NREGS + WHPX_NSREGS], WHPX_NFPU, &v[WHPX_NREGS + WHPX_NSREGS]);

    if (regs) {
        regs->rax    = v[0].Reg64;
        regs->rbx    = v[1].Reg64;
        regs->rcx    = v[2].Reg64;
        regs->rdx    = v[3].Reg64;
        regs->rsi    = v[4].Reg64;
        regs->rdi    = v[5].Reg64;
        regs->rsp    = v[6].Reg64;
        regs->rbp    = v[7].Reg64;
        regs->rip    = v[8].Reg64;
        regs->rflags = v[9].Reg64;
    }
    if (sregs) {
        v       = &whpx_state_vals[WHPX_NREGS];
        segs[0] = &sregs->cs;
        segs[1] = &sregs->ds;
        segs[2] = &sregs->es;
        segs[3] = &sregs->fs;
        segs[4] = &sregs->gs;
        segs[5] = &sregs->ss;
        segs[6] = &sregs->tr;
        segs[7] = &sregs->ldt;
        for (int i = 0; i < 8; i++)
            whpx_seg_from(segs[i], &v[i].Segment);
        sregs->gdt.base  = v[8].Table.Base;
        sregs->gdt.limit = v[8].Table.Limit;
        sregs->idt.base  = v[9].Table.Base;
        sregs->idt.limit = v[9].Table.Limit;
        sregs->cr0       = v[10].Reg64;
        sregs->cr2       = v[11].Reg64;
        sregs->cr3       = v[12].Reg64;
        sregs->cr4       = v[13].Reg64;
    }
    if (fpu) {
        v = &whpx_state_vals[WHPX_NREGS + WHPX_NSREGS];
        for (int i = 0; i < 8; i++)
            memcpy(fpu->fpr[i], &v[i].Fp.AsUINT128, 16);
        fpu->fcw         = v[8].FpControlStatus.FpControl;
        fpu->fsw         = v[8].FpControlStatus.FpStatus;
        fpu->ftwx        = v[8].FpControlStatus.FpTag;
        fpu->last_opcode = v[8].FpControlStatus.LastFpOp;
        fpu->last_ip     = v[8].FpControlStatus.LastFpRip;
        fpu->last_dp     = v[9].XmmControlStatus.LastFpRdp;
    }

    return 0;
}

void
hv_be_set_state(const hv_regs_t *regs, const hv_sregs_t *sregs, const hv_fpu_t *fpu)
{
    WHV_REGISTER_VALUE *v = whpx_state_vals;
    const hv_seg_t     *segs[8];

    if (regs) {
        v[0].Reg64 = regs->rax;
        v[1].Reg64 = regs->rbx;
        v[2].Reg64 = regs->rcx;
        v[3].Reg64 = regs->rdx;
        v[4].Reg64 = regs->rsi;
        v[5].Reg64 = regs->rdi;
        v[6].Reg64 = regs->rsp;
        v[7].Reg64 = regs->rbp;
        v[8].Reg64 = regs->rip;
        v[9].Reg64 = regs->rflags;
        whpx_set(whpx_state_names, WHPX_NREGS, v);

        whpx_rflags = regs->rflags;
    }
    if (sregs) {
        v       = &whpx_state_vals[WHPX_NREGS];
        segs[0] = &sregs->cs;
        segs[1] = &sregs->ds;
        segs[2] = &sregs->es;
        segs[3] = &sregs->fs;
        segs[4] = &sregs->gs;
        segs[5] = &sregs->ss;
        segs[6] = &sregs->tr;
        segs[7] = &sregs->ldt;
        for (int i = 0; i < 8; i++)
            whpx_seg_to(&v[i].Segment, segs[i]);
        memset(&v[8], 0, sizeof(v[8]));
        v[8].Table.Base  = sregs->gdt.base;
        v[8].Table.Limit = sregs->gdt.limit;
        memset(&v[9], 0, sizeof(v[9]));
        v[9].Table.Base  = sregs->idt.base;
        v[9].Table.Limit = sregs->idt.limit;
        v[10].Reg64      = sregs->cr0;
        v[11].Reg64      = sregs->cr2;
        v[12].Reg64      = sregs->cr3;
        v[13].Reg64      = sregs->cr4;
        whpx_set(&whpx_state_names[WHPX_NREGS], WHPX_NSREGS, v);
    }
    if (fpu) {
        v = &whpx_state_vals[WHPX_NREGS + WHPX_NSREGS];
        for (int i = 0; i < 8; i++)
            memcpy(&v[i].Fp.AsUINT128, fpu->fpr[i], 16);
        v[8].FpControlStatus.FpControl = fpu->fcw;
        v[8].FpControlStatus.FpStatus  = fpu->fsw;
        v[8].FpControlStatus.FpTag     = fpu->ftwx;
        v[8].FpControlStatus.LastFpOp  = fpu->last_opcode;
        v[8].FpControlStatus.LastFpRip = fpu->last_ip;
        v[9].XmmControlStatus.LastFpRdp = fpu->last_dp;
        whpx_set(&whpx_state_names[WHPX_NREGS + WHPX_NSREGS], WHPX_NFPU, v);
    }
}

/* Debug registers, which enter_smm() and leave_smm() work with. */
static void
whpx_get_dr(void)
{
    static const WHV_REGISTER_NAME names[2] = { WHvX64RegisterDr6, WHvX64RegisterDr7 };
    WHV_REGISTER_VALUE             vals[2];

    whpx_get(names, 2, vals);
    dr[6] = vals[0].Reg64;
    dr[7] = vals[1].Reg64;
}

static void
whpx_set_dr(void)
{
    static const WHV_REGISTER_NAME names[2] = { WHvX64RegisterDr6, WHvX64RegisterDr7 };
    WHV_REGISTER_VALUE             vals[2];

    memset(vals, 0, sizeof(vals));
    vals[0].Reg64 = dr[6];
    vals[1].Reg64 = dr[7];
    whpx_set(names, 2, vals);
}

/* An exception, back to the guest as it was raised. */
static void
whpx_inject_exception(int vector, int has_error, uint32_t error)
{
    WHV_REGISTER_VALUE v;

    memset(&v, 0, sizeof(v));
    v.PendingInterruption.InterruptionPending = 1;
    v.PendingInterruption.InterruptionType    = WHvX64PendingException;
    v.PendingInterruption.DeliverErrorCode    = !!has_error;
    v.PendingInterruption.ErrorCode           = error;
    v.PendingInterruption.InterruptionVector  = vector;
    whpx_set1(WHvRegisterPendingInterruption, v.Reg64);
    whpx_ev_pending = 1;
}

/* The instruction the guest was stopped at was carried out for it: an
   interrupt shadow it was in (the instruction after STI or MOV SS) is
   over, which WHPX does not know. Left set, an interrupt that is pending
   after STI; OUT; CLI is never taken (the BIOS's INT 16h wait loop). */
static void
whpx_end_shadow(void)
{
    WHV_REGISTER_VALUE v;

    if (!whpx_shadow)
        return;

    v.Reg64 = whpx_get1(WHvRegisterInterruptState);
    if (v.InterruptState.InterruptShadow) {
        v.InterruptState.InterruptShadow = 0;
        whpx_set1(WHvRegisterInterruptState, v.Reg64);
    }
    whpx_shadow = 0;
}

/* The exceptions that stop the guest: #MF always (FERR#), #UD in SMM (RSM),
   #GP on a processor with FXSR but no SSE (FXRSTOR). */
static uint64_t
whpx_exc_always(void)
{
    return (1ULL << WHvX64ExceptionTypeFloatingPointErrorFault) |
           (whpx_fxsr_only ? (1ULL << WHvX64ExceptionTypeGeneralProtectionFault) : 0);
}

static void
whpx_exceptions(void)
{
    WHV_PARTITION_PROPERTY prop;
    uint64_t               want = whpx_exc_always() |
                                  ((in_smm || whpx_exc_fixed) ? (1ULL << WHvX64ExceptionTypeInvalidOpcodeFault) : 0);
    HRESULT                hr;

    if (want == whpx_exc_bitmap)
        return;

    memset(&prop, 0, sizeof(prop));
    prop.ExceptionExitBitmap = want;
    hr = pWHvSetPartitionProperty(whpx_part, WHvPartitionPropertyCodeExceptionExitBitmap, &prop, sizeof(prop));
    if (FAILED(hr)) {
        /* Stuck with what setup had, #UD with it. */
        pclog("WHPX: the exception exit bitmap cannot be changed (%08lX), #UD always stops the guest\n", (unsigned long) hr);
        whpx_exc_fixed = 1;
        return;
    }
    whpx_exc_bitmap = want;
}

/* Memory. */
uint8_t *
hv_be_hma_window(void)
{
    /* No remapping of host memory under WHPX: the slots follow A20. */
    return NULL;
}

void
hv_be_hma_follow(void)
{
}

void
hv_be_zones(UNUSED(uint32_t lo), UNUSED(uint32_t hi))
{
    /* No queued writes under WHPX. */
}

int
hv_be_ready(void)
{
    return whpx_part != NULL;
}

void
hv_be_slot_map(UNUSED(int as), const hv_slot_t *slot)
{
    WHV_MAP_GPA_RANGE_FLAGS flags = WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagExecute;
    HRESULT                 hr;

    if (!slot->ro)
        flags |= WHvMapGpaRangeFlagWrite;
    if (slot->dirty)
        flags |= WHvMapGpaRangeFlagTrackDirtyPages;

    whpx_log("WHPX: map %08X+%X %s %p\n", slot->start << MEM_GRANULARITY_BITS,
             (slot->end - slot->start) << MEM_GRANULARITY_BITS, slot->ro ? "ro" : "rw", slot->host);
    hr = pWHvMapGpaRange(whpx_part, slot->host, (uint64_t) slot->start << MEM_GRANULARITY_BITS,
                         (uint64_t) (slot->end - slot->start) << MEM_GRANULARITY_BITS, flags);
    if (FAILED(hr))
        fatal("WHPX: mapping %08X+%X (host %p) failed: %08lX\n", slot->start << MEM_GRANULARITY_BITS,
              (slot->end - slot->start) << MEM_GRANULARITY_BITS, slot->host, (unsigned long) hr);
}

void
hv_be_slot_unmap(UNUSED(int as), const hv_slot_t *slot)
{
    HRESULT hr;

    whpx_log("WHPX: unmap %08X+%X\n", slot->start << MEM_GRANULARITY_BITS, (slot->end - slot->start) << MEM_GRANULARITY_BITS);
    hr = pWHvUnmapGpaRange(whpx_part, (uint64_t) slot->start << MEM_GRANULARITY_BITS,
                           (uint64_t) (slot->end - slot->start) << MEM_GRANULARITY_BITS);
    if (FAILED(hr))
        fatal("WHPX: unmapping %08X+%X failed: %08lX\n", slot->start << MEM_GRANULARITY_BITS,
              (slot->end - slot->start) << MEM_GRANULARITY_BITS, (unsigned long) hr);
}

void
hv_be_dirty_log(UNUSED(int as), const hv_slot_t *slot, uint64_t *bitmap)
{
    uint32_t pages = slot->end - slot->start;
    HRESULT  hr;

    hr = pWHvQueryGpaRangeDirtyBitmap(whpx_part, (uint64_t) slot->start << MEM_GRANULARITY_BITS,
                                      (uint64_t) pages << MEM_GRANULARITY_BITS, bitmap, ((pages + 63) / 64) * 8);
    if (FAILED(hr))
        whpx_fatal("WHvQueryGpaRangeDirtyBitmap", hr);
}

/* The instruction emulator's view of the machine: 86Box's. */
static HRESULT CALLBACK
whpx_emu_io(UNUSED(void *ctx), WHV_EMULATOR_IO_ACCESS_INFO *io)
{
    uint32_t data = io->Direction ? io->Data : 0;

    hv_io_access(io->Port, io->AccessSize, io->Direction, (uint8_t *) &data, 1);
    if (!io->Direction)
        io->Data = data;
    return S_OK;
}

static HRESULT CALLBACK
whpx_emu_mem(UNUSED(void *ctx), WHV_EMULATOR_MEMORY_ACCESS_INFO *mem)
{
    hv_mmio_access((uint32_t) mem->GpaAddress, mem->Data, mem->AccessSize, mem->Direction);
    return S_OK;
}

static HRESULT CALLBACK
whpx_emu_get(UNUSED(void *ctx), const WHV_REGISTER_NAME *names, UINT32 n, WHV_REGISTER_VALUE *vals)
{
    return pWHvGetVirtualProcessorRegisters(whpx_part, 0, names, n, vals);
}

static HRESULT CALLBACK
whpx_emu_set(UNUSED(void *ctx), const WHV_REGISTER_NAME *names, UINT32 n, const WHV_REGISTER_VALUE *vals)
{
    for (uint32_t i = 0; i < n; i++) {
        if (names[i] == WHvX64RegisterRflags)
            whpx_rflags = vals[i].Reg64;
    }
    return pWHvSetVirtualProcessorRegisters(whpx_part, 0, names, n, vals);
}

static HRESULT CALLBACK
whpx_emu_gva(UNUSED(void *ctx), WHV_GUEST_VIRTUAL_ADDRESS gva, WHV_TRANSLATE_GVA_FLAGS flags,
             WHV_TRANSLATE_GVA_RESULT_CODE *result, WHV_GUEST_PHYSICAL_ADDRESS *gpa)
{
    WHV_TRANSLATE_GVA_RESULT r;
    HRESULT                  hr = pWHvTranslateGva(whpx_part, 0, gva, flags, &r, gpa);

    *result = r.ResultCode;
    return hr;
}

/* An I/O instruction. A plain IN or OUT is carried out here; a string one
   by the emulator. */
static void
whpx_io(void)
{
    const WHV_X64_IO_PORT_ACCESS_CONTEXT *io   = &whpx_exit.IoPortAccess;
    int                                   size = io->AccessInfo.AccessSize;
    WHV_REGISTER_NAME                     names[2] = { WHvX64RegisterRip, WHvX64RegisterRax };
    WHV_REGISTER_VALUE                    vals[2];
    WHV_EMULATOR_STATUS                   st;
    uint32_t                              data;
    uint64_t                              mask;
    HRESULT                               hr;

    if (io->AccessInfo.StringOp) {
        hr = pWHvEmulatorTryIoEmulation(whpx_emu, NULL, &whpx_exit.VpContext, io, &st);
        if (FAILED(hr) || !st.EmulationSuccessful) {
            whpx_stat_emu_fail++;
            if (whpx_stat_emu_fail <= 4)
                pclog("WHPX: the instruction emulator failed (%08lX, status %08X) at %04X:%08llX, string I/O to port %04X, "
                      "%02X %02X %02X %02X\n",
                      (unsigned long) hr, SUCCEEDED(hr) ? st.AsUINT32 : 0, whpx_exit.VpContext.Cs.Selector,
                      (unsigned long long) whpx_exit.VpContext.Rip, io->PortNumber, io->InstructionBytes[0],
                      io->InstructionBytes[1], io->InstructionBytes[2], io->InstructionBytes[3]);
            hv_emulate();
        }
        whpx_end_shadow();
        return;
    }

    data = (uint32_t) io->Rax;
    hv_io_access(io->PortNumber, size, io->AccessInfo.IsWrite, (uint8_t *) &data, 1);

    memset(vals, 0, sizeof(vals));
    vals[0].Reg64 = whpx_exit.VpContext.Rip + whpx_exit.VpContext.InstructionLength;
    if (io->AccessInfo.IsWrite)
        whpx_set(names, 1, vals);
    else {
        mask          = (size == 4) ? 0xffffffffULL : ((1ULL << (size * 8)) - 1);
        vals[1].Reg64 = (size == 4) ? data : ((io->Rax & ~mask) | (data & mask));
        whpx_set(names, 2, vals);
    }
    whpx_end_shadow();
}

/* An access to memory that is not mapped, or to read only memory. */
static void
whpx_mmio(void)
{
    const WHV_MEMORY_ACCESS_CONTEXT *ma = &whpx_exit.MemoryAccess;
    WHV_EMULATOR_STATUS              st;
    HRESULT                          hr;

    /* Code that runs from a device's memory. */
    if (ma->AccessInfo.AccessType == WHvMemoryAccessExecute) {
        hv_emulate();
        whpx_end_shadow();
        return;
    }

    hr = pWHvEmulatorTryMmioEmulation(whpx_emu, NULL, &whpx_exit.VpContext, ma, &st);
    if (SUCCEEDED(hr) && st.EmulationSuccessful) {
        whpx_end_shadow();
        return;
    }

    /* An event the guest could not take into read only memory, pinned
       shadow RAM: mapped as it is from now on, and taken again. */
    if ((whpx_exit.VpContext.ExecutionState.InterruptionPending || (SUCCEEDED(hr) && st.InterruptCausedIntercept)) &&
        hv_pin_failed(ma->Gpa))
        return;

    whpx_stat_emu_fail++;
    if (whpx_stat_emu_fail <= 4)
        pclog("WHPX: the instruction emulator failed (%08lX, status %08X) at %04X:%08llX, GPA %08llX\n",
              (unsigned long) hr, SUCCEEDED(hr) ? st.AsUINT32 : 0, whpx_exit.VpContext.Cs.Selector,
              (unsigned long long) whpx_exit.VpContext.Rip, (unsigned long long) ma->Gpa);
    hv_emulate();
    whpx_end_shadow();
}

/* MSRs that WHPX does not handle itself: the emulated processor's. */
static void
whpx_msr(void)
{
    const WHV_X64_MSR_ACCESS_CONTEXT *msr   = &whpx_exit.MsrAccess;
    int                               write = msr->AccessInfo.IsWrite;
    uint64_t                          data  = ((msr->Rdx & 0xffffffffULL) << 32) | (msr->Rax & 0xffffffffULL);
    WHV_REGISTER_NAME                 names[3] = { WHvX64RegisterRip, WHvX64RegisterRax, WHvX64RegisterRdx };
    WHV_REGISTER_VALUE                vals[3];

    if (hv_msr_access(msr->MsrNumber, write, &data)) {
        whpx_log("WHPX: %sMSR %08X #GP\n", write ? "WR" : "RD", msr->MsrNumber);
        whpx_inject_exception(13, 1, 0);
        return;
    }
    whpx_log("WHPX: %sMSR %08X %016" PRIX64 "\n", write ? "WR" : "RD", msr->MsrNumber, data);

    memset(vals, 0, sizeof(vals));
    vals[0].Reg64 = whpx_exit.VpContext.Rip + whpx_exit.VpContext.InstructionLength;
    vals[1].Reg64 = (uint32_t) data;
    vals[2].Reg64 = (uint32_t) (data >> 32);
    whpx_set(names, write ? 1 : 3, vals);
    whpx_end_shadow();
}

/* CPUID: the emulated processor's, less what the host cannot do. */
static uint32_t whpx_host1_edx;

static void
whpx_cpuid_leaf(uint32_t leaf, uint32_t sub, uint32_t out[4])
{
    uint32_t old_eax = EAX;
    uint32_t old_ebx = EBX;
    uint32_t old_ecx = ECX;
    uint32_t old_edx = EDX;

    EAX = leaf;
    EBX = EDX = 0;
    ECX = sub;
    cpu_CPUID();
    out[0] = EAX;
    out[1] = EBX;
    out[2] = ECX;
    out[3] = EDX;

    EAX = old_eax;
    EBX = old_ebx;
    ECX = old_ecx;
    EDX = old_edx;
}

static void
whpx_cpuid(void)
{
    const WHV_X64_CPUID_ACCESS_CONTEXT *c    = &whpx_exit.CpuidAccess;
    uint32_t                            leaf = (uint32_t) c->Rax;
    uint32_t                            r[4];
    WHV_REGISTER_NAME                   names[5] = { WHvX64RegisterRip, WHvX64RegisterRax, WHvX64RegisterRbx,
                                                     WHvX64RegisterRcx, WHvX64RegisterRdx };
    WHV_REGISTER_VALUE                  vals[5];

    whpx_cpuid_leaf(leaf, (uint32_t) c->Rcx, r);
    if (leaf == 0x00000001) {
        r[2] &= (uint32_t) c->DefaultResultRcx;
        r[3] &= (uint32_t) c->DefaultResultRdx;
    } else if (leaf == 0x80000001) {
        r[2] &= (uint32_t) c->DefaultResultRcx;
        r[3] &= (uint32_t) c->DefaultResultRdx | (whpx_host1_edx & 0x0183f3ff); /* the bits AMD repeats from leaf 1 */
    }

    memset(vals, 0, sizeof(vals));
    vals[0].Reg64 = whpx_exit.VpContext.Rip + whpx_exit.VpContext.InstructionLength;
    for (int i = 0; i < 4; i++)
        vals[i + 1].Reg64 = r[i];
    whpx_set(names, 5, vals);
    whpx_end_shadow();
}

/* The leaves to stop the guest at: all of the emulated processor's, and
   the hypervisor's own range, which it does not have. */
static uint32_t
whpx_cpuid_list(UINT32 *list, uint32_t max)
{
    uint32_t r[4];
    uint32_t limit0;
    uint32_t limit8;
    uint32_t n = 0;

    whpx_cpuid_leaf(0x00000000, 0, r);
    limit0 = MIN(r[0], 0x1f);
    whpx_cpuid_leaf(0x80000000, 0, r);
    limit8 = ((r[0] & 0xffff0000) == 0x80000000) ? MIN(r[0], 0x80000020) : 0x80000000;

    for (uint32_t leaf = 0; (leaf <= limit0) && (n < max); leaf++)
        list[n++] = leaf;
    for (uint32_t leaf = 0x40000000; (leaf <= 0x40000010) && (n < max); leaf++)
        list[n++] = leaf;
    for (uint32_t leaf = 0x80000000; (leaf <= limit8) && (n < max); leaf++)
        list[n++] = leaf;

    return n;
}

/* The guest's TSC on 86Box's clock: behind the host's by as much as the
   guest is. The offset it has is read back each time, which keeps the
   error of a write from adding up, and follows the guest's own writes. */
static uint64_t
whpx_tsc_khz(void)
{
    uint64_t ns = hv_clock_ns() - whpx_tsc_ns0;

    if (ns < 1000000000ULL)
        return whpx_tsc_khz0;

    return (uint64_t) (((uint128_t) (__rdtsc() - whpx_tsc_host0) * 1000000ULL) / ns);
}

static void
whpx_tsc_sync(void)
{
    uint64_t khz   = whpx_tsc_khz();
    int64_t  ticks = (int64_t) (((uint128_t) hv_behind_ns * khz) / 1000000ULL);
    int64_t  slack = MAX((int64_t) (khz / 200), (int64_t) (4 * whpx_tsc_cost)); /* 5 us, or a few writes' worth */
    uint64_t h0;
    uint64_t h1;
    uint64_t g;
    int64_t  base;

    if (llabs(ticks - whpx_tsc_applied) < slack)
        return;

    h0   = __rdtsc();
    g    = whpx_get1(WHvX64RegisterTsc);
    h1   = __rdtsc();
    base = (int64_t) (g - ((h0 + h1) / 2)) + whpx_tsc_applied;

    h0 = __rdtsc();
    whpx_set1(WHvX64RegisterTsc, h0 + (whpx_tsc_cost / 2) + (uint64_t) (base - ticks));
    h1 = __rdtsc();
    whpx_tsc_cost += ((int64_t) (h1 - h0) - (int64_t) whpx_tsc_cost) / 4;

    whpx_tsc_applied = ticks;
    whpx_stat_tsc++;
}

static void
whpx_tsc_init(void)
{
    uint64_t h0 = __rdtsc();
    uint64_t t0 = hv_clock_ns();
    uint64_t t1;

    /* A rough rate to begin with, a better one from a second on. */
    do
        t1 = hv_clock_ns();
    while ((t1 - t0) < 10000000ULL);
    whpx_tsc_khz0    = (uint64_t) (((uint128_t) (__rdtsc() - h0) * 1000000ULL) / (t1 - t0));
    whpx_tsc_host0   = h0;
    whpx_tsc_ns0     = t0;
    whpx_tsc_applied = 0;
    whpx_tsc_cost    = 0;
    pclog("WHPX: host TSC at about %" PRIu64 " kHz\n", whpx_tsc_khz0);
}

/* State. */
static void
whpx_seg_reset(WHV_REGISTER_VALUE *v, uint16_t sel, uint32_t base, uint16_t attr)
{
    memset(v, 0, sizeof(WHV_REGISTER_VALUE));
    v->Segment.Selector   = sel;
    v->Segment.Base       = base;
    v->Segment.Limit      = 0xffff;
    v->Segment.Attributes = attr;
}

/* The processor's state after RESET or INIT, as reset_common() left it in
   cpu_state, into the virtual processor. */
static void
whpx_apply_reset(void)
{
    WHV_REGISTER_NAME  names[40];
    WHV_REGISTER_VALUE vals[40];
    int                n = 0;

#define WHPX_R(name, val)                 \
    do {                                  \
        names[n] = name;                  \
        memset(&vals[n], 0, sizeof(vals[n])); \
        vals[n++].Reg64 = (val);          \
    } while (0)
    WHPX_R(WHvX64RegisterRax, 0);
    WHPX_R(WHvX64RegisterRbx, 0);
    WHPX_R(WHvX64RegisterRcx, 0);
    WHPX_R(WHvX64RegisterRdx, EDX);
    WHPX_R(WHvX64RegisterRsi, 0);
    WHPX_R(WHvX64RegisterRdi, 0);
    WHPX_R(WHvX64RegisterRsp, 0);
    WHPX_R(WHvX64RegisterRbp, 0);
    WHPX_R(WHvX64RegisterRip, 0x0000fff0);
    WHPX_R(WHvX64RegisterRflags, 0x00000002);
    WHPX_R(WHvX64RegisterCr0, cpu_state.CR0.l | 0x10);
    WHPX_R(WHvX64RegisterCr2, 0);
    WHPX_R(WHvX64RegisterCr3, 0);
    WHPX_R(WHvX64RegisterCr4, 0);
    WHPX_R(WHvX64RegisterEfer, 0);
    WHPX_R(WHvX64RegisterDr6, 0xffff0ff0);
    WHPX_R(WHvX64RegisterDr7, 0x00000400);
    WHPX_R(WHvRegisterPendingInterruption, 0);
    WHPX_R(WHvRegisterInterruptState, 0);
    WHPX_R(WHvX64RegisterDeliverabilityNotifications, 0);
#undef WHPX_R

    names[n] = WHvX64RegisterCs;
    whpx_seg_reset(&vals[n++], 0xf000, 0xffff0000, 0x9b);
    names[n] = WHvX64RegisterDs;
    whpx_seg_reset(&vals[n++], 0x0000, 0x00000000, 0x93);
    names[n] = WHvX64RegisterEs;
    whpx_seg_reset(&vals[n++], 0x0000, 0x00000000, 0x93);
    names[n] = WHvX64RegisterFs;
    whpx_seg_reset(&vals[n++], 0x0000, 0x00000000, 0x93);
    names[n] = WHvX64RegisterGs;
    whpx_seg_reset(&vals[n++], 0x0000, 0x00000000, 0x93);
    names[n] = WHvX64RegisterSs;
    whpx_seg_reset(&vals[n++], 0x0000, 0x00000000, 0x93);
    names[n] = WHvX64RegisterLdtr;
    whpx_seg_reset(&vals[n++], 0x0000, 0x00000000, 0x82);
    names[n] = WHvX64RegisterTr;
    whpx_seg_reset(&vals[n++], 0x0000, 0x00000000, 0x8b);
    names[n] = WHvX64RegisterGdtr;
    memset(&vals[n], 0, sizeof(vals[n]));
    vals[n++].Table.Limit = 0xffff;
    names[n] = WHvX64RegisterIdtr;
    memset(&vals[n], 0, sizeof(vals[n]));
    vals[n++].Table.Limit = 0xffff;

    if (hv_reset_pending > 1) {
        /* RESET: the FPU as well, which INIT leaves alone. */
        names[n] = WHvX64RegisterFpControlStatus;
        memset(&vals[n], 0, sizeof(vals[n]));
        vals[n++].FpControlStatus.FpControl = 0x037f;
        names[n] = WHvX64RegisterXmmControlStatus;
        memset(&vals[n], 0, sizeof(vals[n]));
        vals[n++].XmmControlStatus.XmmStatusControl = 0x1f80;
        for (int i = 0; i < 8; i++) {
            names[n] = WHvX64RegisterFpMmx0 + i;
            memset(&vals[n++], 0, sizeof(vals[0]));
        }
    }

    whpx_set(names, n, vals);

    whpx_rflags      = 0x00000002;
    whpx_shadow      = 0;
    whpx_ev_pending  = 0;
    whpx_notify      = 0;
    hv_halted        = 0;
    hv_reset_pending = 0;
    whpx_exceptions();
}

/* FERR#. With CR0.NE clear, the x87 reports an error on FERR#, which the
   board turns into IRQ 13 (x87_ferr.c), and IGNNE# lets it go on past it;
   VMX keeps CR0.NE set, so the processor raises #MF instead, which stops
   the guest before it is delivered. The instruction is run by the CPU
   interpreter, which reports the error as the board does, or with IGNNE#
   runs it. Until the error is taken, the processor waits at the
   instruction for an interrupt. */
static int
whpx_ferr_same(const hv_fpu_t *a, const hv_fpu_t *b)
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

static void
whpx_ferr(void)
{
    hv_state_t st;
    uint32_t   pc;

    hv_state_begin(&st);
    /* An FPU that is no longer as it was when FERR# went up had its error
       cleared since: this one is new. */
    if (fpu_ferr && !whpx_ferr_same(&st.fpu, &whpx_ferr_snap))
        x87_ferr_clear();

    pc = cpu_state.pc;
    exec386_hv_step();
    if ((cpu_state.pc == pc) && !(cpu_state.CR0.l & 0x20))
        hv_halted = 2;
    hv_state_end(&st);
    whpx_ferr_snap = st.fpu;
    if (++whpx_stat_ferr <= 4)
        pclog("WHPX: x87 error at %04X:%08X reported on FERR#%s\n", CS, pc, (cpu_state.pc == pc) ? "" : ", ignored (IGNNE#)");
}

/* FERR# goes down, IGNNE# with it, as ES clears. */
static void
whpx_ferr_poll(void)
{
    WHV_REGISTER_NAME  name = WHvX64RegisterFpControlStatus;
    WHV_REGISTER_VALUE v;

    if (!fpu_ferr)
        return;
    whpx_get(&name, 1, &v);
    if (!(v.FpControlStatus.FpStatus & 0x80))
        x87_ferr_clear();
}

/* SMM, 86Box's: an SMI runs enter_smm() on the processor's state. */
static void
whpx_smi(void)
{
    hv_state_t st;
    int        in_hlt = hv_smi_halted;

    hv_smi_halted = 0;
    if (!is_am486 && !is_pentium && !is_k5 && !is_k6 && !is_p6 && !is_cxsmm) {
        smi_line = 0;
        return;
    }

    /* Latched until RSM: nothing of the processor's changes. */
    if (in_smm && !is_gxm) {
        enter_smm_check(in_hlt);
        return;
    }

    hv_state_begin(&st);
    whpx_get_dr();
    enter_smm_check(in_hlt);
    hv_state_end(&st);
    whpx_set_dr();
    whpx_ev_pending = 0;
    whpx_exceptions();
    whpx_stat_smi++;
    whpx_log("WHPX: SMI, %s SMM\n", in_smm ? "in" : "not in");
}

/* RSM, caught as the #UD that VMX raises for it. */
static void
whpx_rsm(void)
{
    hv_state_t st;

    hv_state_begin(&st);
    whpx_get_dr();

    hv_rsm = 1;
    leave_smm();
    hv_rsm = 0;

    if (smi_latched) {
        /* The latched SMI is taken at once (opRSM()). enter_smm() steps over
           the HLT of an auto HALT restart, where the CPU emulation's EIP
           would be; WHPX's is past it already. */
        if (smm_in_hlt)
            cpu_state.pc--;
        enter_smm(smm_in_hlt);
    } else if (smm_in_hlt)
        hv_halted = 1; /* auto HALT restart, EIP past the HLT */

    hv_state_end(&st);
    whpx_set_dr();
    whpx_exceptions();
    whpx_log("WHPX: RSM, %s SMM, %s\n", in_smm ? "in" : "out of", hv_halted ? "halted" : "running");
}

/* FXRSTOR on a processor with FXSR but no SSE (Pentium II, Celeron
   Mendocino): the MXCSR field of its image is reserved there, and an
   operating system that does not use SSE leaves anything in it. Windows XP
   copies it from frame to frame for user mode callbacks, a stale stack
   value included. The host's processor has SSE and, with CR4.OSFXSR set,
   raises #GP for reserved MXCSR bits (bugcheck 0x7F, 0x0D in KiTrap07).
   The CPU interpreter runs the instruction as the emulated processor does. */
static int
whpx_is_fxrstor(const UINT8 *b, int n)
{
    int i = 0;

    /* Prefixes: segment, operand and address size, LOCK, REP. */
    while ((i < n) && ((b[i] == 0x26) || (b[i] == 0x2e) || (b[i] == 0x36) || (b[i] == 0x3e) || (b[i] == 0x64) ||
                       (b[i] == 0x65) || (b[i] == 0x66) || (b[i] == 0x67) || (b[i] == 0xf0) || (b[i] == 0xf2) ||
                       (b[i] == 0xf3)))
        i++;

    /* 0F AE /1, a memory operand. */
    return ((i + 2) < n) && (b[i] == 0x0f) && (b[i + 1] == 0xae) && ((b[i + 2] & 0x38) == 0x08) &&
           ((b[i + 2] & 0xc0) != 0xc0);
}

static void
whpx_fxrstor(void)
{
    hv_state_t st;
    uint32_t   pc;

    hv_state_begin(&st);
    pc = cpu_state.pc;
    exec386_hv_step();
    hv_state_end(&st);
    whpx_end_shadow();
    if (++whpx_stat_fxrstor <= 4)
        pclog("WHPX: FXRSTOR at %04X:%08X raised #GP on the host's processor, run as the emulated one does\n", CS, pc);
}

static void
whpx_exception(void)
{
    const WHV_VP_EXCEPTION_CONTEXT *e = &whpx_exit.VpException;

    switch (e->ExceptionType) {
        case WHvX64ExceptionTypeFloatingPointErrorFault:
            if ((fpu_type != FPU_NONE) && !(whpx_get1(WHvX64RegisterCr0) & 0x20)) {
                whpx_ferr();
                return;
            }
            break;

        case WHvX64ExceptionTypeInvalidOpcodeFault:
            if (in_smm && (e->InstructionByteCount >= 2) && (e->InstructionBytes[0] == 0x0f) &&
                (e->InstructionBytes[1] == 0xaa)) {
                whpx_rsm();
                return;
            }
            break;

        case WHvX64ExceptionTypeGeneralProtectionFault:
            if (whpx_fxsr_only && whpx_is_fxrstor(e->InstructionBytes, e->InstructionByteCount)) {
                whpx_fxrstor();
                return;
            }
            break;

        default:
            break;
    }

    whpx_inject_exception(e->ExceptionType, e->ExceptionInfo.ErrorCodeValid, e->ErrorCode);
}

/* SMIs, NMIs and interrupts for the next entry. */
static void
whpx_events(void)
{
    WHV_REGISTER_NAME  names[2];
    WHV_REGISTER_VALUE vals[2];
    uint64_t           notify = 0;
    int                n      = 0;

    /* Not while an event is on its way in: it would be taken in SMM. */
    if (smi_line && !whpx_ev_pending)
        whpx_smi();

    if (hv_nmi_pending()) {
        WHV_REGISTER_VALUE is;

        is.Reg64 = whpx_ev_pending ? 0 : whpx_get1(WHvRegisterInterruptState);
        if (!whpx_ev_pending && !is.InterruptState.NmiMasked) {
            names[n] = WHvRegisterPendingInterruption;
            memset(&vals[n], 0, sizeof(vals[n]));
            vals[n].PendingInterruption.InterruptionPending = 1;
            vals[n].PendingInterruption.InterruptionType    = WHvX64PendingNmi;
            vals[n].PendingInterruption.InterruptionVector  = 2;
            n++;
            whpx_ev_pending = 1;
            nmi             = 0;
        } else
            notify |= 1; /* NmiNotification */
    }

    if (x86_external_interrupt_pending()) {
        if (!whpx_ev_pending && (whpx_rflags & 0x200) && !whpx_shadow) {
            int vector = x86_external_interrupt_vector();

            if (vector != -1) {
                names[n] = WHvRegisterPendingInterruption;
                memset(&vals[n], 0, sizeof(vals[n]));
                vals[n].PendingInterruption.InterruptionPending = 1;
                vals[n].PendingInterruption.InterruptionType    = WHvX64PendingInterrupt;
                vals[n].PendingInterruption.InterruptionVector  = vector;
                n++;
                whpx_ev_pending = 1;
                whpx_stat_irq++;
            }
            /* Another one behind it. */
            if (x86_external_interrupt_pending())
                notify |= 2; /* InterruptNotification */
        } else {
            notify |= 2;
            whpx_stat_irq_wait++;
        }
    }

    if (notify != whpx_notify) {
        names[n] = WHvX64RegisterDeliverabilityNotifications;
        memset(&vals[n], 0, sizeof(vals[n]));
        vals[n].DeliverabilityNotifications.NmiNotification       = !!(notify & 1);
        vals[n].DeliverabilityNotifications.InterruptNotification = !!(notify & 2);
        n++;
        whpx_notify = notify;
    }

    if (n)
        whpx_set(names, n, vals);
}

/* Once a call: what the guest changes without stopping, paging (for pinned
   shadow RAM). */
static void
whpx_poll(void)
{
    hv_pin_poll(whpx_get1(WHvX64RegisterCr0));
    whpx_ferr_poll();
}

int
hv_available(void)
{
    BOOL                present = FALSE;
    WHV_CAPABILITY      cap;
    UINT32              size;
    const char         *why = NULL;

    if (hv_avail >= 0)
        return hv_avail;

    hv_avail = 0;
    if ((whpx_dll = dynld_module("WinHvPlatform.dll", whpx_imports)) == NULL)
        why = "WinHvPlatform.dll could not be loaded";
    else if ((whpx_emu_dll = dynld_module("WinHvEmulation.dll", whpx_emu_imports)) == NULL)
        why = "WinHvEmulation.dll could not be loaded";
    else if (FAILED(pWHvGetCapability(WHvCapabilityCodeHypervisorPresent, &present, sizeof(present), &size)) || !present)
        why = "the hypervisor is not running";
    else {
        memset(&cap, 0, sizeof(cap));
        if (FAILED(pWHvGetCapability(WHvCapabilityCodeExtendedVmExits, &cap, sizeof(cap), &size)) ||
            !cap.ExtendedVmExits.X64CpuidExit || !cap.ExtendedVmExits.X64MsrExit || !cap.ExtendedVmExits.ExceptionExit)
            why = "it cannot stop the guest at CPUID, MSR accesses and exceptions";
        else {
            memset(&cap, 0, sizeof(cap));
            if (FAILED(pWHvGetCapability(WHvCapabilityCodeFeatures, &cap, sizeof(cap), &size)) || !cap.Features.DirtyPageTracking)
                why = "it cannot track the pages the guest writes";
            else
                hv_avail = 1;
        }
    }

    if (!hv_avail) {
        pclog("WHPX: not available (%s), the CPU is emulated\n", why);
        /* Said once a session: the machine runs on, emulated. */
        warning("This machine is set to run its CPU on the Windows Hypervisor Platform, which is not available (%s). "
                "Turn on the \"Windows Hypervisor Platform\" Windows feature, and virtualization in the firmware "
                "setup, then restart Windows. The CPU is emulated for now.", why);
    }

    return hv_avail;
}

static void
whpx_init(void)
{
    WHV_PARTITION_PROPERTY prop;
    WHV_EMULATOR_CALLBACKS cbs;
    UINT32                 leaves[96];
    uint32_t               nleaves;
    uint32_t               a = 0;
    uint32_t               b = 0;
    uint32_t               c = 0;
    uint32_t               d = 0;
    LARGE_INTEGER          f;
    HRESULT                hr;

    QueryPerformanceFrequency(&f);
    whpx_qpc_freq = f.QuadPart;

    if (FAILED(hr = pWHvCreatePartition(&whpx_part)))
        whpx_fatal("WHvCreatePartition", hr);

    memset(&prop, 0, sizeof(prop));
    prop.ProcessorCount = 1;
    if (FAILED(hr = pWHvSetPartitionProperty(whpx_part, WHvPartitionPropertyCodeProcessorCount, &prop, sizeof(prop))))
        whpx_fatal("setting the processor count", hr);

    memset(&prop, 0, sizeof(prop));
    prop.ExtendedVmExits.X64CpuidExit  = 1;
    prop.ExtendedVmExits.X64MsrExit    = 1;
    prop.ExtendedVmExits.ExceptionExit = 1;
    if (FAILED(hr = pWHvSetPartitionProperty(whpx_part, WHvPartitionPropertyCodeExtendedVmExits, &prop, sizeof(prop))))
        whpx_fatal("setting the extended exits", hr);

    /* FXSR (EDX bit 24) without SSE (bit 25). */
    whpx_cpuid_leaf(0x00000001, 0, leaves);
    whpx_fxsr_only = (leaves[3] & 0x03000000) == 0x01000000;

    /* #UD too until it is seen that it can be dropped afterwards. */
    memset(&prop, 0, sizeof(prop));
    prop.ExceptionExitBitmap = whpx_exc_always() | (1ULL << WHvX64ExceptionTypeInvalidOpcodeFault);
    if (FAILED(hr = pWHvSetPartitionProperty(whpx_part, WHvPartitionPropertyCodeExceptionExitBitmap, &prop, sizeof(prop))))
        whpx_fatal("setting the exception exits", hr);
    whpx_exc_bitmap = prop.ExceptionExitBitmap;
    whpx_exc_fixed  = 0;

    nleaves = whpx_cpuid_list(leaves, sizeof(leaves) / sizeof(leaves[0]));
    if (FAILED(hr = pWHvSetPartitionProperty(whpx_part, WHvPartitionPropertyCodeCpuidExitList, leaves, nleaves * sizeof(UINT32))))
        whpx_fatal("setting the CPUID exits", hr);
    __get_cpuid(1, &a, &b, &c, &d);
    whpx_host1_edx = d;

    if (FAILED(hr = pWHvSetupPartition(whpx_part)))
        whpx_fatal("WHvSetupPartition", hr);
    if (FAILED(hr = pWHvCreateVirtualProcessor(whpx_part, 0, 0)))
        whpx_fatal("WHvCreateVirtualProcessor", hr);
    whpx_vp = 1;

    memset(&cbs, 0, sizeof(cbs));
    cbs.Size                                    = sizeof(cbs);
    cbs.WHvEmulatorIoPortCallback               = whpx_emu_io;
    cbs.WHvEmulatorMemoryCallback               = whpx_emu_mem;
    cbs.WHvEmulatorGetVirtualProcessorRegisters = whpx_emu_get;
    cbs.WHvEmulatorSetVirtualProcessorRegisters = whpx_emu_set;
    cbs.WHvEmulatorTranslateGvaPage             = whpx_emu_gva;
    if (FAILED(hr = pWHvEmulatorCreateEmulator(&cbs, &whpx_emu)))
        whpx_fatal("WHvEmulatorCreateEmulator", hr);

    pclog("WHPX: initializing, %u CPUID leaves of the emulated processor's\n", nleaves);

    /* The whole memory map, in the one address space. */
    hv_as_count = 1;
    hv_mem_init();

    /* The thread that stops the guest. */
    whpx_hrt      = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    whpx_hrt_main = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!whpx_hrt || !whpx_hrt_main) {
        /* Before Windows 10 1803: a coarser timer. */
        pclog("WHPX: no high resolution timers, the guest's timers are late\n");
        if (!whpx_hrt)
            whpx_hrt = CreateWaitableTimerW(NULL, FALSE, NULL);
        if (!whpx_hrt_main)
            whpx_hrt_main = CreateWaitableTimerW(NULL, FALSE, NULL);
    }
    whpx_wake     = CreateEventW(NULL, FALSE, FALSE, NULL);
    whpx_deadline = 0;
    whpx_quit     = 0;
    whpx_slack_ns = 500000;
    whpx_thread   = CreateThread(NULL, 0, whpx_timer_thread, NULL, 0, NULL);
    if (!whpx_thread)
        fatal("WHPX: creating the timer thread failed: %lu\n", GetLastError());
    SetThreadPriority(whpx_thread, THREAD_PRIORITY_TIME_CRITICAL);

    hv_ns_last   = hv_clock_ns();
    hv_lag_ns    = 0;
    hv_behind_ns = 0;
    hv_tsc_frac  = 0;
    whpx_tsc_init();
    hv_cpuclock = 0.0;
    hv_rate_update();

    hv_halted        = 0;
    hv_reset_pending = 2;
    whpx_apply_reset();
    if (in_smm)
        hv_smm_bus(0);
}

void
hv_close(void)
{
    if (whpx_thread) {
        whpx_quit = 1;
        SetEvent(whpx_wake);
        WaitForSingleObject(whpx_thread, INFINITE);
        CloseHandle(whpx_thread);
        whpx_thread = NULL;
    }
    if (whpx_wake) {
        CloseHandle(whpx_wake);
        whpx_wake = NULL;
    }
    if (whpx_hrt) {
        CloseHandle(whpx_hrt);
        whpx_hrt = NULL;
    }
    if (whpx_hrt_main) {
        CloseHandle(whpx_hrt_main);
        whpx_hrt_main = NULL;
    }
    if (whpx_emu) {
        pWHvEmulatorDestroyEmulator(whpx_emu);
        whpx_emu = NULL;
    }
    if (whpx_vp) {
        pWHvDeleteVirtualProcessor(whpx_part, 0);
        whpx_vp = 0;
    }
    if (whpx_part) {
        pWHvDeletePartition(whpx_part);
        whpx_part = NULL;
    }
    hv_nslots[0] = hv_nslots[1] = 0;
}

/* A run of the guest that ended well past the deadline meant to stop it:
   the guest's clocks jumped by as much. Said, the first few times, every
   power of ten, and once a second at most for any of 5 ms or more. */
static void
whpx_stall_check(uint64_t deadline)
{
    static uint64_t logged;
    int64_t         late = (int64_t) (hv_ns_last - deadline);

    if (late < (int64_t) WHPX_STALL_NS)
        return;

    whpx_stat_late++;
    if ((whpx_stat_late > 4) && (whpx_stat_late != whpx_stat_late_next) &&
        ((late < 5000000LL) || ((int64_t) (hv_ns_last - logged) < 1000000000LL)))
        return;
    if (whpx_stat_late >= whpx_stat_late_next)
        whpx_stat_late_next *= 10;
    logged = hv_ns_last;
    pclog("WHPX: the guest stood still for %.1f ms (%" PRIu64 " so far)\n", late / 1000000.0, whpx_stat_late);
}

static void
whpx_stats(void)
{
#ifdef ENABLE_WHPX_LOG
    static uint64_t next = 0;

    if ((int64_t) (hv_ns_last - next) < 0)
        return;
    if (next) {
        whpx_log("WHPX: exits io %" PRIu64 " mmio %" PRIu64 " hlt %" PRIu64 " canceled %" PRIu64 " irqwin %" PRIu64
                 " msr %" PRIu64 " cpuid %" PRIu64 " exc %" PRIu64 " emulator failures %" PRIu64 " TSC writes %" PRIu64
                 " SMIs %" PRIu64 " timer slack %" PRId64 " us interrupts %" PRIu64 " (%" PRIu64 " waiting) min run %" PRIu64 " us stop late %" PRIu64 " us early timers %" PRIu64 "\n",
                 whpx_stat_exits[2], whpx_stat_exits[1], whpx_stat_exits[8], whpx_stat_exits[15], whpx_stat_exits[7],
                 whpx_stat_exits[10], whpx_stat_exits[11], whpx_stat_exits[12], whpx_stat_emu_fail, whpx_stat_tsc,
                 whpx_stat_smi, (int64_t) whpx_slack_ns / 1000, whpx_stat_irq, whpx_stat_irq_wait, hv_min_run_ns / 1000, whpx_late_ns / 1000, whpx_stat_early);
        memset(whpx_stat_exits, 0, sizeof(whpx_stat_exits));
        whpx_stat_tsc = whpx_stat_irq = whpx_stat_irq_wait = whpx_stat_early = 0;
    }
    next = hv_ns_last + 5000000000ULL;
#endif
}

static inline int
whpx_stat_index(WHV_RUN_VP_EXIT_REASON r)
{
    switch (r) {
        case WHvRunVpExitReasonX64MsrAccess:
            return 10;
        case WHvRunVpExitReasonX64Cpuid:
            return 11;
        case WHvRunVpExitReasonException:
            return 12;
        case WHvRunVpExitReasonCanceled:
            return 15;
        default:
            return (r < 10) ? r : 13;
    }
}

/* A precise timer (TIMER_PRECISE) that is due sooner than the timer thread
   could stop the guest is run now: the guest's clock is moved on to it,
   out of the lag it is owed, so that its interrupt is there as the guest
   runs again rather than hundreds of microseconds late. The BIOS's timer
   tests count on that (Award's POST 18h: PIT channel 0 counting 8, STI,
   and the interrupt expected within a LOOP of 7000h). */
static void
whpx_fire_early(void)
{
    uint64_t          early = MIN(whpx_late_ns + whpx_out_ns, WHPX_MAX_EARLY_NS);
    const pc_timer_t *t;
    uint64_t          gap;

    for (t = timer_head; t != NULL; t = t->next) {
        gap = hv_ts_to_host(t->ts_integer) - hv_ns_last;
        if ((int64_t) gap > (int64_t) early)
            return;
        if (t->flags & TIMER_PRECISE)
            break;
    }
    if ((t == NULL) || ((int64_t) gap <= 0) || (gap > hv_lag_ns))
        return;

    hv_lag_ns -= gap;
    hv_behind_ns -= gap;
    hv_advance(gap);
    whpx_stat_early++;
}

void
hv_exec(UNUSED(int32_t cycs))
{
    uint64_t end;
    uint64_t deadline;
    uint64_t run_end = 0;
    HRESULT  hr;

    if (!whpx_part)
        whpx_init();

    hv_rate_update();

    /* The time since the last call, in the main loop or paused, is lag. */
    hv_account_out();
    end = hv_ns_last + (force_10ms ? (10 * HV_QUANTUM_NS) : HV_QUANTUM_NS);

    /* What the guest wrote into a device's memory since the last call, for
       the device to see before its timers next look. */
    hv_dirty_all();

    whpx_poll();

    while (1) {
        hv_account_out();
        whpx_fire_early();
        hv_timers();

        /* A reset some devices leave for the CPU loop (pci_trc_reset()). */
        if (cpu_init) {
            cpu_init = 0;
            resetx86();
        }

        if (hv_reset_pending)
            whpx_apply_reset();

        if (hv_halted) {
            if (!hv_halt(end, !!(whpx_rflags & 0x200)))
                break;
            run_end = 0; /* the wait is not the exit's */
        }

        if ((int64_t) (hv_ns_last - end) >= 0)
            break;

        whpx_events();
        hv_mem_flush();

        /* Everything up to here, the mapping changes above included, was
           time outside the guest. */
        hv_account_out();
        deadline = hv_stop_deadline();
        if ((int64_t) (deadline - end) > 0)
            deadline = end;
        whpx_arm(deadline);
        whpx_tsc_sync();

        /* Only cycles of devices, not of the processor, are counted now. */
        cycles = 0;

        /* What the last exit took to handle, up to here, and to come out of
           the guest for. */
        if (run_end) {
            whpx_out_ns += ((int64_t) (hv_ns_last - run_end) - (int64_t) whpx_out_ns) / 16;
            hv_min_run_ns = MIN(MAX((whpx_out_ns + whpx_late_ns) * WHPX_RUN_FACTOR, HV_MIN_RUN_NS), WHPX_MAX_RUN_NS);
        }

        hr = pWHvRunVirtualProcessor(whpx_part, 0, &whpx_exit, sizeof(whpx_exit));
        hv_account_run(0);
        run_end = hv_ns_last;
        whpx_stall_check(deadline);
        if (FAILED(hr))
            whpx_fatal("WHvRunVirtualProcessor", hr);

        whpx_rflags     = whpx_exit.VpContext.Rflags;
        whpx_shadow     = whpx_exit.VpContext.ExecutionState.InterruptShadow;
        whpx_ev_pending = whpx_exit.VpContext.ExecutionState.InterruptionPending;
        whpx_stat_exits[whpx_stat_index(whpx_exit.ExitReason)]++;

        if (whpx_exit.ExitReason != WHvRunVpExitReasonCanceled)
            whpx_disarm_past();
        else if ((int64_t) (hv_ns_last - deadline) > 0)
            whpx_late_ns += ((int64_t) MIN(hv_ns_last - deadline, WHPX_STALL_NS) - (int64_t) whpx_late_ns) / 16;

        /* The devices as they are at the time of the access. */
        hv_timers();

        /* FERR# down as the IRQ 13 handler clears the error. */
        whpx_ferr_poll();

        switch (whpx_exit.ExitReason) {
            case WHvRunVpExitReasonX64IoPortAccess:
                whpx_io();
                break;

            case WHvRunVpExitReasonMemoryAccess:
                whpx_mmio();
                break;

            case WHvRunVpExitReasonX64Halt:
                /* STI; HLT: the shadow ended with the HLT. */
                whpx_end_shadow();
                hv_halted = 1;
                break;

            case WHvRunVpExitReasonX64InterruptWindow:
                /* The notification is spent. */
                whpx_notify = 0;
                break;

            case WHvRunVpExitReasonCanceled:
                break;

            case WHvRunVpExitReasonX64MsrAccess:
                whpx_msr();
                break;

            case WHvRunVpExitReasonX64Cpuid:
                whpx_cpuid();
                break;

            case WHvRunVpExitReasonException:
                whpx_exception();
                break;

            case WHvRunVpExitReasonUnrecoverableException:
                /* Triple fault. */
                hv_dump("triple fault");
                softresetx86();
                cpu_set_edx();
                break;

            case WHvRunVpExitReasonInvalidVpRegisterValue:
                hv_dump("invalid register value");
                fatal("WHPX: Invalid register value\n");
                break;

            case WHvRunVpExitReasonUnsupportedFeature:
                hv_dump("unsupported feature");
                fatal("WHPX: Unsupported feature %d (%016llX)\n", whpx_exit.UnsupportedFeature.FeatureCode,
                      (unsigned long long) whpx_exit.UnsupportedFeature.FeatureParameter);
                break;

            default:
                hv_dump("unknown exit");
                fatal("WHPX: Unknown exit %08X\n", whpx_exit.ExitReason);
                break;
        }

        whpx_stats();
    }
}
