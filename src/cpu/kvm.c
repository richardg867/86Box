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
#include <stdatomic.h>
#include <inttypes.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kvm.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/nmi.h>
#include <86box/pic.h>
#include <86box/timer.h>
#include <86box/thread.h>
#include <86box/hypervisor.h>
#include <86box/plat_unused.h>
#undef cs
#undef ds
#undef es
#undef ss
#undef gs
#undef cr0

#define ENABLE_KVM_LOG 1
#ifdef ENABLE_KVM_LOG
int kvm_do_log = ENABLE_KVM_LOG;
extern bool fast_forward;
void
kvm_log(const char *fmt, ...)
{
    va_list ap;

    if (fast_forward||kvm_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define kvm_log(fmt, ...)
#endif

#ifdef THREADED_TIMERS
static thread_t     *thread;
static mutex_t      *mutex;
static ATOMIC_INT    run_thread = 0;
static ATOMIC_UINT64 timeout_ns = 0;
#else
static timer_t           timeout_timer;
static struct itimerspec timeout_latch = { .it_interval = { .tv_nsec = 100000000ULL } }; /* 100ms repeat to prevent deadlocks */
#endif

static int kvm_fd = -1;
static int vm_fd  = -1;
static int cpu_fd = -1;

static int              run_size;
static struct kvm_run  *run = MAP_FAILED;
static struct kvm_regs  regs;
static struct kvm_sregs sregs;

static int                                 mem_slots    = 0;
static int                                 mapping_idx  = -1;
static struct kvm_userspace_memory_region *mem_mappings = NULL;

extern int nmi_enable;
extern double cpuclock;

#ifdef THREADED_TIMERS
static void
kvm_thread(UNUSED(void *priv))
{
    struct timespec ts;

    while (1) {
        /* Sleep until the timer target. */
        ts.tv_sec  = timer_realtime_target / 1000000000ULL;
        ts.tv_nsec = timer_realtime_target % 1000000000ULL;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);

        /* Stop if requested. */
        if (!ATOMIC_LOAD(run_thread))
            break;

        /* Process timers. */
        thread_wait_mutex(mutex);
        timer_process();

        /* Pause CPU execution if any interrupts need to be injected. */
        if ((smi_line&&0) || (nmi && nmi_enable && nmi_mask)) {
            atomic_store(&run->immediate_exit, 1);
        } else if (pic.int_pending) {
            kvm_log("KVM: Requesting interrupt window from thread (fl=%llX)\n", regs.rflags);
            atomic_store(&run->request_interrupt_window, 1);
        }
        thread_release_mutex(mutex);
    }
}
#else
static void
kvm_alarm(int sig)
{
    atomic_store(&run->immediate_exit, 1);
}
#endif

static inline void
get_segment(x86seg *dest, struct kvm_segment *src)
{
    dest->base = src->base;
    dest->limit = src->limit;
    dest->seg = src->selector;
    dest->access = (src->type & 0x0f) | (src->s ? 0x40 : 0x00) | ((src->dpl & 0x03) << 5) | (src->present ? 0x80 : 0);
    uint32_t limit20 = src->g ? (src->limit >> 12) : src->limit;
    dest->limit_low  = limit20 & 0xffff;
    dest->limit_high = (limit20 >> 16) & 0xf;
    dest->ar_high = (src->avl ? 0x01 : 0x00) | (src->l ? 0x02 : 0x00) | (src->db ? 0x04 : 0x00) | (src->g ? 0x08 : 0x00);
}

void
hv_get_regs(void)
{
    EAX = regs.rax;
    EBX = regs.rbx;
    ECX = regs.rcx;
    EDX = regs.rdx;
    ESI = regs.rsi;
    EDI = regs.rdi;
    ESP = regs.rsp;
    EBP = regs.rbp;
    cpu_state.pc = regs.rip;
    cpu_state.flags = regs.rflags;
    cpu_state.eflags = regs.rflags >> 16;

    get_segment(&cpu_state.seg_cs, &sregs.cs);
    get_segment(&cpu_state.seg_ds, &sregs.ds);
    get_segment(&cpu_state.seg_es, &sregs.es);
    get_segment(&cpu_state.seg_ss, &sregs.ss);
    get_segment(&cpu_state.seg_fs, &sregs.fs);
    get_segment(&cpu_state.seg_gs, &sregs.gs);
    get_segment(&tr, &sregs.tr);
    get_segment(&ldt, &sregs.ldt);
    gdt.base = sregs.gdt.base;
    gdt.limit = sregs.gdt.limit;
    idt.base = sregs.idt.base;
    idt.limit = sregs.idt.limit;

    cpu_state.CR0.l = sregs.cr0;
    cr2 = sregs.cr2;
    cr3 = sregs.cr3;
    cr4 = sregs.cr4;
    msr.amd_efer = sregs.efer;
    msr.apic_base = sregs.apic_base;
}

static inline void
apply_segment(struct kvm_segment *dest, x86seg *src)
{
    dest->base = src->base;
    dest->limit = src->limit;
    dest->selector = src->seg;
    dest->type = src->access & 0x0f;
    dest->s = !!(src->access & 0x10);
    dest->dpl = (src->access >> 5) & 0x03;
    dest->present = !!(src->access & 0x80);
    dest->avl = !!(src->ar_high & 0x01);
    dest->l = !!(src->ar_high & 0x02);
    dest->db = !!(src->ar_high & 0x04);
    dest->g = !!(src->ar_high & 0x08);
    uint32_t limit20 = ((uint32_t) (src->ar_high & 0xf0) << 12) | src->limit_low;
    dest->limit = dest->g ? ((limit20 << 12) | 0xfff) : limit20;
    dest->unusable = (!dest->present && dest->s);
}

void
hv_apply_regs(void)
{
    regs.rax = EAX;
    regs.rbx = EBX;
    regs.rcx = ECX;
    regs.rdx = EDX;
    regs.rsi = ESI;
    regs.rdi = EDI;
    regs.rsp = ESP;
    regs.rbp = EBP;
    regs.rip = cpu_state.pc;
    regs.rflags = cpu_state.flags | ((uint32_t) cpu_state.eflags << 16);

    apply_segment(&sregs.cs, &cpu_state.seg_cs);
    apply_segment(&sregs.ds, &cpu_state.seg_ds);
    apply_segment(&sregs.es, &cpu_state.seg_es);
    apply_segment(&sregs.ss, &cpu_state.seg_ss);
    apply_segment(&sregs.fs, &cpu_state.seg_fs);
    apply_segment(&sregs.gs, &cpu_state.seg_gs);
    apply_segment(&sregs.tr, &tr);
    apply_segment(&sregs.ldt, &ldt);
    sregs.gdt.base = gdt.base;
    sregs.gdt.limit = gdt.limit;
    sregs.idt.base = idt.base;
    sregs.idt.limit = idt.limit;

    sregs.cr0 = cpu_state.CR0.l;
    sregs.cr2 = cr2;
    sregs.cr3 = cr3;
    sregs.cr4 = cr4;
    sregs.efer = msr.amd_efer;
    sregs.apic_base = msr.apic_base;

    if (UNLIKELY(ioctl(cpu_fd, KVM_SET_REGS, &regs) < 0))
        fatal("KVM: SET_REGS failed: %s\n", strerror(errno));
    if (UNLIKELY(ioctl(cpu_fd, KVM_SET_SREGS, &sregs) < 0))
        fatal("KVM: SET_SREGS failed: %s\n", strerror(errno));
}

void
hv_recalc_mappings(uint32_t base, uint32_t size)
{
    if (vm_fd == -1)
        return; /* not initialized yet */

    pclog("KVM: Recalculating mappings at %08X+%X\n", base, size);

    /* Clear all existing mappings. */
    if (mapping_idx >= 0) {
        struct kvm_userspace_memory_region empty = { 0 };
        for (; empty.slot <= mapping_idx; empty.slot++) {
            if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &empty) < 0)
                fatal("KVM: Clear memory mapping slot %d failed: %s\n", empty.slot, strerror(errno));
        }
    }

    /* Calculate new mappings. */
    mem_mapping_t *mapping = NULL;
    mapping_idx = -1;
    for (uint64_t addr = 0; addr < 0x100000000ULL; addr += MEM_GRANULARITY_SIZE) {
        if (mem_addr_is_ram(addr) ||
            (read_mapping[addr >> MEM_GRANULARITY_BITS] && (read_mapping[addr >> MEM_GRANULARITY_BITS]->flags & MEM_MAPPING_ROMCS))) {
            if (read_mapping[addr >> MEM_GRANULARITY_BITS] != mapping) {
                mapping = read_mapping[addr >> MEM_GRANULARITY_BITS];
                kvm_log("KVM: Found memory mapping %08X+%X at %08X\n", mapping->base, mapping->size, addr);
                if (++mapping_idx >= mem_slots)
                    fatal("KVM: Memory mapping overflow\n");
                memset(&mem_mappings[mapping_idx], 0, sizeof(struct kvm_userspace_memory_region));
                mem_mappings[mapping_idx].slot            = mapping_idx;
                mem_mappings[mapping_idx].guest_phys_addr = addr;
                mem_mappings[mapping_idx].memory_size     = MEM_GRANULARITY_SIZE;
                mem_mappings[mapping_idx].userspace_addr  = (uint64_t) &mapping->exec[addr - mapping->base];
                if (mapping->flags & MEM_MAPPING_ROM)
                    mem_mappings[mapping_idx].flags = KVM_MEM_READONLY;
            } else {
                mem_mappings[mapping_idx].memory_size += MEM_GRANULARITY_SIZE;
            }
        } else {
            mapping = NULL;
        }
    }

    for (int i = 0; i <= mapping_idx; i++) {
        if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &mem_mappings[i]) < 0)
            fatal("KVM: Memory mapping %08llX+%llX failed: %s\n", mem_mappings[i].guest_phys_addr, mem_mappings[i].memory_size, strerror(errno));
        else
            pclog("KVM: Memory mapping %08llX+%llX successful\n", mem_mappings[i].guest_phys_addr, mem_mappings[i].memory_size);
    }

    kvm_log("KVM: Memory mapped\n");
}

void
hv_init(void)
{
    kvm_log("KVM: Initializing\n");

    /* Open KVM container. */
    kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm_fd < 0)
        fatal("KVM: Unable to open: %s\n", strerror(errno));

    /* Check for API version. */
    int api = ioctl(kvm_fd, KVM_GET_API_VERSION, 0);
    if (api < 12)
        fatal("KVM: Unknown API version %d\n", api);

    if (ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_USER_MEMORY) <= 0)
        fatal("KVM: User memory extension not available\n");

    /* Get memory slot count. */
    mem_slots = ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_NR_MEMSLOTS);
    if (mem_slots < 3)
        fatal("Not enough memory slots (%d)\n", mem_slots);
    kvm_log("KVM: %d memory slots\n", mem_slots);
    if (mem_slots > 256)
        mem_slots = 256;
    mem_mappings = malloc(mem_slots * sizeof(struct kvm_userspace_memory_region));

    /* Create virtual machine. */
    vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
    if (vm_fd < 0)
        fatal("KVM: CREATE_VM failed: %s\n", strerror(errno));

    /* Enable detailed emulation error exits. */
    struct kvm_enable_cap cap = { .cap = KVM_CAP_EXIT_ON_EMULATION_FAILURE };
    ioctl(vm_fd, KVM_ENABLE_CAP, &cap);

    /* Enable user space MSRs. */
    cap.cap = KVM_CAP_X86_USER_SPACE_MSR;
    cap.args[0] = KVM_MSR_EXIT_REASON_UNKNOWN | KVM_MSR_EXIT_REASON_INVAL;
    if (ioctl(vm_fd, KVM_ENABLE_CAP, &cap) < 0)
        fatal("KVM: ENABLE_CAP X86_USER_SPACE_MSR failed: %s\n", strerror(errno));

    /* Create initial memory mappings. */
    hv_recalc_mappings(0, -1);

    /* Create virtual CPU. */
    cpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
    if (cpu_fd < 0)
        fatal("KVM: CREATE_VCPU failed: %s\n", strerror(errno));

    /* Set CPUID. */
    struct kvm_cpuid2 *cpuid;
    size_t cpuid_slots = 11;
    while (1) {
        cpuid = calloc(1, sizeof(struct kvm_cpuid2) + (cpuid_slots * sizeof(struct kvm_cpuid_entry2)));
        cpuid->nent = cpuid_slots;
        int ret = ioctl(kvm_fd, KVM_GET_SUPPORTED_CPUID, cpuid);
        if (((ret < 0) && (errno == E2BIG)) || (cpuid->nent >= cpuid_slots)) {
            free(cpuid);
            cpuid_slots++;
        } else if (ret == 0) {
            break;
        } else {
            fatal("KVM: GET_SUPPORTED_CPUID failed: %s\n", strerror(errno));
        }
    }
#ifndef USE_HOST_CPUID
    uint32_t flags0 = -1;
    uint32_t flags8 = -1;
#endif
    for (int i = 0; i < cpuid->nent; i++) {
#ifdef USE_HOST_CPUID
        if ((cpuid->entries[i].function >= 0x80000002) || (cpuid->entries[i].function <= 0x80000005)) {
                asm volatile("cpuid"
                             : "=a"(cpuid->entries[i].eax), "=b"(cpuid->entries[i].ebx),
                               "=c"(cpuid->entries[i].ecx), "=d"(cpuid->entries[i].edx)
                             : "0"(cpuid->entries[i].function), "c"(0) : "cc");
        }
#else
        if (cpuid->entries[i].function == 1)
            flags0 = cpuid->entries[i].edx;
        else if (cpuid->entries[i].function == 0x80000001)
            flags8 = cpuid->entries[i].edx;
#endif
    }
#ifndef USE_HOST_CPUID
    free(cpuid);
    EAX = EBX = ECX = EDX = 0;
    cpu_CPUID();
    uint32_t limit0 = EAX;
    EAX = 0x80000000;
    EBX = ECX = EDX = 0;
    cpu_CPUID();
    uint32_t limit8 = EAX;
    cpuid_slots = limit0 + 1;
    if (limit8 >= 0x80000000)
        cpuid_slots += (limit8 - 0x80000000) + 1;
    cpuid = calloc(1, sizeof(struct kvm_cpuid2) + (cpuid_slots * sizeof(struct kvm_cpuid_entry2)));
    cpuid->nent = 0;
    for (uint32_t i = 0; i <= limit8; i++) {
        EAX = cpuid->entries[cpuid->nent].function = i;
        EBX = ECX = EDX = 0;
        cpu_CPUID();
        if (i == 1)
            EDX &= flags0;
        else if (i == 0x80000001)
            EDX &= (flags8 != (uint32_t) -1) ? flags8 : flags0;
        cpuid->entries[cpuid->nent].eax = EAX;
        cpuid->entries[cpuid->nent].ebx = EBX;
        cpuid->entries[cpuid->nent].ecx = ECX;
        cpuid->entries[cpuid->nent].edx = EDX;
        cpuid->nent++;
        if (i == limit0)
            i = 0x80000000 - 1;
    }
#endif
    //if (ioctl(cpu_fd, KVM_SET_CPUID2, cpuid) < 0)
    //    fatal("KVM: SET_CPUID2 failed: %s\n", strerror(errno));
    free(cpuid);

    /* Allocate memory for run structure. */
    int run_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    run = mmap(0, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, cpu_fd, 0);
    if (run == MAP_FAILED)
        fatal("KVM: mmap failed\n");

    /* Start execution thread. */
#ifdef THREADED_TIMERS
    mutex      = thread_create_mutex();
    run_thread = 1;
    thread     = thread_create(kvm_thread, NULL);
#else
    struct sigaction sa = {
        .sa_flags = SA_SIGINFO,
        .sa_handler = kvm_alarm
    };
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGALRM, &sa, NULL) < 0)
        fatal("KVM: sigaction failed: %s\n", strerror(errno));
    if (timer_create(CLOCK_MONOTONIC, NULL, &timeout_timer) < 0)
        fatal("KVM: timer_create failed: %s\n", strerror(errno));
#endif
}

void
hv_close(void)
{
#ifdef THREADED_TIMERS
    kvm_log("KVM: Waiting for thread to end...\n");
    ATOMIC_STORE(run_thread, 0);
    thread_wait(thread);
    kvm_log("KVM: Done\n");
#endif

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
    if (mem_mappings) {
        free(mem_mappings);
        mem_mappings = NULL;
    }
}

void
hv_exec(int32_t cycs)
{
    if (kvm_fd == -1)
        hv_init();

    uint64_t start = timer_get_clock_ns();
    uint64_t current = start;
    do {
#ifdef THREADED_TIMERS
        int ret = ioctl(cpu_fd, KVM_RUN, 0);
        int err = errno;
        thread_wait_mutex(mutex);
#else
        pclog("arming timer for %lld ns\n", (int64_t) (timer_realtime_target - current));
        timeout_latch.it_value.tv_sec  = timer_realtime_target / 1000000000ULL;
        timeout_latch.it_value.tv_nsec = timer_realtime_target % 1000000000ULL;
        atomic_store(&run->immediate_exit, 0);
        atomic_thread_fence(memory_order_seq_cst);
        timer_settime(timeout_timer, TIMER_ABSTIME, &timeout_latch, NULL);

        int ret = ioctl(cpu_fd, KVM_RUN, 0);
        int err = errno;
#endif
        if (LIKELY(ret >= 0)) {
            /* Dump registers. */
            if (UNLIKELY(ioctl(cpu_fd, KVM_GET_REGS, &regs) < 0))
                fatal("KVM: GET_REGS failed: %s\n", strerror(errno));
            if (UNLIKELY(ioctl(cpu_fd, KVM_GET_SREGS, &sregs) < 0))
                fatal("KVM: GET_SREGS failed: %s\n", strerror(errno));
            kvm_log("KVM: Exit at %04X:%08llX ax=%04llX bx=%04llX dx=%04llX\n", sregs.cs.selector, regs.rip, regs.rax, regs.rbx, regs.rdx);

            /* Determine exit reason. */
            switch (run->exit_reason) {
                case KVM_EXIT_INTERNAL_ERROR:
                    for (int i = 0; i < run->internal.ndata; i++)
                        pclog("KVM: data[%d] = %016" PRIx64 "\n", i, (uint64_t) run->internal.data[i]);
                    fatal("KVM: Internal error %08X\n", (uint32_t) run->internal.suberror);
                    break;

                case KVM_EXIT_SHUTDOWN:
                    fatal("KVM: Triple fault\n");
                    break;

                case KVM_EXIT_FAIL_ENTRY:
                    fatal("KVM: Fail entry %016llX\n", (uint64_t) run->fail_entry.hardware_entry_failure_reason);
                    break;

                case KVM_EXIT_HLT:
                    pclog("KVM: Exit by hlt\n");
                    break;

                case KVM_EXIT_IO:
                    kvm_log("KVM: Exit by %s%c(%04X)*%d\n", (run->io.direction == KVM_EXIT_IO_OUT) ? "out" : "in", (run->io.size >= 4) ? 'l' : ((run->io.size >= 2) ? 'w' : 'b'), run->io.port, run->io.count);

                    if (run->io.size >= 4) {
                        uint32_t *data = (uint32_t *) &((uint8_t *) run)[run->io.data_offset];
                        if (run->io.direction == KVM_EXIT_IO_OUT) {
                            for (uint32_t i = 0; i < run->io.count; i++)
                                outl(run->io.port, data[i]);
                        } else {
                            for (uint32_t i = 0; i < run->io.count; i++)
                                data[i] = inl(run->io.port);
                        }
                    } else if (run->io.size >= 2) {
                        uint16_t *data = (uint16_t *) &((uint8_t *) run)[run->io.data_offset];
                        if (run->io.direction == KVM_EXIT_IO_OUT) {
                            for (uint32_t i = 0; i < run->io.count; i++)
                                outw(run->io.port, data[i]);
                        } else {
                            for (uint32_t i = 0; i < run->io.count; i++)
                                data[i] = inw(run->io.port);
                        }
                    } else {
                        uint8_t *data = &((uint8_t *) run)[run->io.data_offset];
                        if (run->io.direction == KVM_EXIT_IO_OUT) {
                            for (uint32_t i = 0; i < run->io.count; i++)
                                outb(run->io.port, data[i]);
                        } else {
                            for (uint32_t i = 0; i < run->io.count; i++)
                                data[i] = inb(run->io.port);
                        }
                    }
                    break;

                case KVM_EXIT_MMIO:
                    kvm_log("KVM: Exit by mem_%s%c_phys(%08X)\n", run->mmio.is_write ? "write" : "read", (run->mmio.len >= 8) ? 'q' : ((run->mmio.len >= 4) ? 'l' : ((run->mmio.len >= 2) ? 'w' : 'b')), (uint32_t) run->mmio.phys_addr);

                    if (run->mmio.len >= 4) {
                        if (run->mmio.is_write) {
                            mem_writel_phys(run->mmio.phys_addr, AS_U32(run->mmio.data[0]));
                            if (run->mmio.len >= 8)
                                mem_writel_phys(run->mmio.phys_addr + 4, AS_U32(run->mmio.data[4]));
                        } else {
                            AS_U32(run->mmio.data[0]) = mem_readl_phys(run->mmio.phys_addr);
                            if (run->mmio.len >= 8)
                                AS_U32(run->mmio.data[4]) = mem_readl_phys(run->mmio.phys_addr + 4);
                        }
                    } else if (run->mmio.len >= 2) {
                        if (run->mmio.is_write)
                            mem_writew_phys(run->mmio.phys_addr, AS_U16(run->mmio.data[0]));
                        else
                            AS_U16(run->mmio.data[0]) = mem_readw_phys(run->mmio.phys_addr);
                    } else {
                        if (run->mmio.is_write)
                            mem_writeb_phys(run->mmio.phys_addr, run->mmio.data[0]);
                        else
                            run->mmio.data[0] = mem_readb_phys(run->mmio.phys_addr);
                    }
                    break;

                case KVM_EXIT_IRQ_WINDOW_OPEN:
                    kvm_log("KVM: Exit by interrupt window (pending=%d iflag=%d)\n", pic.int_pending, !!(regs.rflags & I_FLAG));
                    run->request_interrupt_window = 0;
                    break;

                case KVM_EXIT_X86_RDMSR:
                    fatal("unknown rdmsr %08X\n", run->msr.index);
                    break;

                case KVM_EXIT_X86_WRMSR:
                    fatal("unknown wrmsr %08X %016llX\n", run->msr.index, run->msr.data);
                    break;

                default:
                    fatal("KVM: Unknown exit %d\n", run->exit_reason);
                    break;
            }
        } else if (err != EINTR) {
            fatal("KVM: RUN failed: %s\n", strerror(err));
        } else {
            if (UNLIKELY(ioctl(cpu_fd, KVM_GET_REGS, &regs) < 0))
                fatal("KVM: GET_REGS failed: %s\n", strerror(errno));
            if (UNLIKELY(ioctl(cpu_fd, KVM_GET_SREGS, &sregs) < 0))
                fatal("KVM: GET_SREGS failed: %s\n", strerror(errno));
            kvm_log("KVM: Timeout at %04X:%08llX ax=%04llX bx=%04llX dx=%04llX", sregs.cs.selector, regs.rip, regs.rax, regs.rbx, regs.rdx);
            kvm_log("\n");
        }

        if (smi_line&&0) {
            enter_smm_check(0);
        } else if (nmi && nmi_enable && nmi_mask) {
            nmi_enable = 0;
#    ifdef OLD_NMI_BEHAVIOR
            if (nmi_auto_clear) {
                nmi_auto_clear = 0;
                nmi            = 0;
            }
#    else
            nmi = 0;
#    endif
            if (UNLIKELY(ioctl(cpu_fd, KVM_NMI, &regs) < 0))
                fatal("KVM: NMI failed: %s\n", strerror(errno));
        } else if (pic.int_pending) {
            if (run->ready_for_interrupt_injection) {
                int vector = picinterrupt();
                if (LIKELY(vector != -1)) {
                    kvm_log("KVM: Injecting interrupt vector %02X\n", vector);
                    struct kvm_interrupt interrupt = { .irq = vector };
                    if (UNLIKELY(ioctl(cpu_fd, KVM_INTERRUPT, &interrupt) < 0))
                        fatal("KVM: INTERRUPT failed: %s\n", strerror(errno));
                } else {
                    pclog("KVM: Interrupt window requested but no vector found\n");
                }
            } else {
                kvm_log("KVM: Requesting interrupt window from main loop\n");
                run->request_interrupt_window = 1;
            }
        }

        tsc += (__uint128_t) (current - timer_clock_last) * cpuclock / 1000000000ULL;
        timer_clock_last = current;
#ifdef THREADED_TIMERS
        thread_release_mutex(mutex);
#else
        if ((int64_t) (timer_realtime_target - current) <= 0)
            timer_process();
#endif
        current = timer_get_clock_ns();
    } while ((current - start) < 1000000ULL);
}
