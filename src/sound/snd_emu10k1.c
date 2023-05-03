/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Creative EMU10K1 (SB Live) audio controller emulation.
 *
 *          Based on the emu10k1 Linux driver written by
 *          Jaroslav Kysela <perex@perex.cz> and potentially Creative.
 *
 *
 *
 * Authors: RichardG, <richardg867@gmail.com>
 *
 *          Copyright 2023 RichardG.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <inttypes.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/timer.h>
#include <86box/nmi.h>
#include <86box/pci.h>
#include <86box/gameport.h>
#include <86box/sound.h>
#include <86box/snd_ac97.h>
#include <86box/snd_emu8k.h>
#include <86box/snd_mpu401.h>

enum {
    EMU10K1 = 0x0002,
    EMU10K2 = 0x0004,
};
enum {
    SB_LIVE_CT4670 = 0x0020,
    SB_LIVE_CT4620 = 0x0021,
    SB_LIVE_CT4780 = 0x8022,
    SB_LIVE_CT4760 = 0x8024,
    SB_LIVE_SB0060 = 0x8061,
    SB_LIVE_SB0220 = 0x8065,
};

enum {
    TRAP_DMA1 = 0,
    TRAP_DMA2,
    TRAP_PIC1,
    TRAP_PIC2,
    TRAP_SB,
    TRAP_OPL,
    TRAP_MPU,
    TRAP_MAX
};

static const struct {
    const int type;
    const uint16_t id;
    const device_t *codec;
} emu10k1_models[] = {
    {
        .type = EMU10K1,
        .id = SB_LIVE_CT4670,
        .codec = NULL /* CT1297 */
    }, {
        .type = EMU10K1,
        .id = SB_LIVE_CT4620,
        .codec = NULL /* CT1297 */
    }, {
        .type = EMU10K1,
        .id = SB_LIVE_CT4780,
        .codec = &cs4297a_device
    }, {
        .type = EMU10K1,
        .id = SB_LIVE_CT4760,
        .codec = &stac9721_device
    }, {
        .type = EMU10K1,
        .id = SB_LIVE_SB0060,
        .codec = &stac9708_device
    }, {
        .type = EMU10K1,
        .id = SB_LIVE_SB0220,
        .codec = &stac9708_device
    }
};

typedef struct {
    struct _emu10k1_ *dev;
    void *trap;
    uint8_t flag;
} emu10k1_io_trap_t;

typedef struct _emu10k1_ {
    emu8k_t emu8k; /* at the beginning so we can cast back */

    int type, slot;
    uint16_t id, io_base;

    uint8_t pci_regs[256], pci_game_regs[256], io_regs[32];
    uint32_t indirect_regs[4096];
    int timer_interval, timer_count;

    uint32_t pages[8192], pagemask;
    uint16_t tlb[256];
    uint8_t tlb_pos; /* clamped by type! */

    struct {
        int64_t acc; /* 67-bit in hardware */
        uint32_t regs[256], etram_mask;
        uint16_t tram_frac[256], /* lower bits of TRAM address only visible to the DSP */
                 itram[8192]; /* internal TRAM */
        int skip, stop: 1, interrupt: 1;
    } dsp;

    pc_timer_t poll_timer;
    uint64_t timer_latch;

    ac97_codec_t *codec;
    mpu_t mpu[2];
    void *gameport;
    emu10k1_io_trap_t io_traps[TRAP_MAX];

    int master_vol_l, master_vol_r, pcm_vol_l, pcm_vol_r, cd_vol_l, cd_vol_r;
} emu10k1_t;

#define ENABLE_EMU10K1_LOG 1
#ifdef ENABLE_EMU10K1_LOG
int emu10k1_do_log = ENABLE_EMU10K1_LOG;

static void
emu10k1_log(const char *fmt, ...)
{
    va_list ap;

    if (emu10k1_do_log > 0) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}

#    define emu10k1_log_push() emu10k1_do_log--
#    define emu10k1_log_pop()  emu10k1_do_log++
#else
#    define emu10k1_log(fmt, ...)
#    define emu10k1_log_push()
#    define emu10k1_log_pop()
#endif

#define EMU10K1_MMU_UNMAPPED ((uint32_t) -1)
#define EMU10K1_TLB_UNCACHED ((uint16_t) -1)

static const uint32_t dsp_constants[] = {
    0x00000000, 0x00000001, 0x00000002, 0x00000003, 0x00000004, 0x00000008, 0x00000010, 0x00000020,
    0x00000100, 0x00010000, 0x00080000, 0x10000000, 0x20000000, 0x40000000, 0x80000000, 0x7fffffff,
    0xffffffff, 0xfffffffe, 0xc0000000, 0x4f1bbcdc, 0x5a7ef9db, 0x00100000
};

static void emu10k1_update_irqs(emu10k1_t *dev);
static uint16_t emu10k1_readw(uint16_t addr, void *priv);
static uint32_t emu10k1_readl(uint16_t addr, void *priv);
static void emu10k1_writew(uint16_t addr, uint16_t val, void *priv);
static void emu10k1_writel(uint16_t addr, uint32_t val, void *priv);

static __inline int32_t
emu10k1_dsp_saturate(emu10k1_t *dev, int64_t i) {
    uint32_t ret;
    if (i > 2147483647) {
        ret = 2147483647;
saturated:
        /* Set saturation flag. */
        dev->dsp.regs[0x57] |= 0x10; /* S */
    } else if (i < -2147483648) {
        ret = -2147483648;
        goto saturated;
    } else {
        ret = i;
    }
    return ret;
}

static __inline int64_t
emu10k1_dsp_add(emu10k1_t *dev, int64_t a, int64_t b)
{
    /* The borrow flag follows this truth table:
       1) a + b = always set
       2) a + -b = a < abs(b)
       3) -a + b = b < abs(a)
       4) -a + -b = never set */
    if (((a >= 0) && (b >= 0)) ||
        ((a >= 0) && (b < 0) && (a < abs(b))) ||
        ((a < 0) && (b >= 0) && (b < abs(a))))
        dev->dsp.regs[0x57] |= 0x02; /* B */
    return a + b;
}

static int32_t
emu10k1_dsp_opMACS(emu10k1_t *dev, int64_t a, int32_t x, int32_t y)
{
    dev->dsp.acc = emu10k1_dsp_add(dev, a, (((uint64_t) x * y) >> 31));
    return emu10k1_dsp_saturate(dev, dev->dsp.acc);
}

static int32_t
emu10k1_dsp_opMACS1(emu10k1_t *dev, int64_t a, int32_t x, int32_t y)
{
    dev->dsp.acc = emu10k1_dsp_add(dev, a, (((uint64_t) -x * y) >> 31));
    return emu10k1_dsp_saturate(dev, dev->dsp.acc);
}

static int32_t
emu10k1_dsp_opMACW(emu10k1_t *dev, int64_t a, int32_t x, int32_t y)
{
    dev->dsp.acc = emu10k1_dsp_add(dev, a, (((uint64_t) x * y) >> 31));
    return dev->dsp.acc;
}

static int32_t
emu10k1_dsp_opMACW1(emu10k1_t *dev, int64_t a, int32_t x, int32_t y)
{
    dev->dsp.acc = emu10k1_dsp_add(dev, a, (((uint64_t) -x * y) >> 31));
    return dev->dsp.acc;
}

static int32_t
emu10k1_dsp_opMACINTS(emu10k1_t *dev, int64_t a, int32_t x, int32_t y)
{
    /* MACINT operations have weird borrow flag handling, seemingly a >= 0 */
    dev->dsp.acc = a + ((uint64_t) x * y);
    if (a >= 0)
        dev->dsp.regs[0x57] |= 0x02; /* B */
    /* MACINT operations set the accumulator to the result's upper 32 bits. */
    int64_t ret = dev->dsp.acc;
    dev->dsp.acc >>= 32;
    return emu10k1_dsp_saturate(dev, ret);
}

static int32_t
emu10k1_dsp_opMACINTW(emu10k1_t *dev, int64_t a, int32_t x, int32_t y)
{
    dev->dsp.acc = a + ((uint64_t) x * y);
    if (a >= 0)
        dev->dsp.regs[0x57] |= 0x02; /* B */
    int64_t ret = dev->dsp.acc;
    dev->dsp.acc >>= 32;
    return ret & 0x7fffffff;
}

static int32_t
emu10k1_dsp_opACC3(emu10k1_t *dev, int64_t a, int32_t x, int32_t y)
{
    /* Borrow check only performed when adding a to x+y.
       The accumulator's lower 32 bits are used, despite documentation.
       Saturation happens at the accumulator. */
    dev->dsp.acc = emu10k1_dsp_saturate(dev, emu10k1_dsp_add(dev, a, x + y));
    return dev->dsp.acc;
}

static int32_t
emu10k1_dsp_opMACMV(emu10k1_t *dev, int64_t a, int32_t x, int32_t y)
{
    /* Clearing up unclear documentation:
       - The order is MAC *then* move.
       - The multiplication result is shifted like MACS/MACW, then saturated. */
    dev->dsp.acc = emu10k1_dsp_saturate(dev, emu10k1_dsp_add(dev, dev->dsp.acc, ((uint64_t) x * y) >> 31));
    return a;
}

static int32_t
emu10k1_dsp_opANDXOR(emu10k1_t *dev, int64_t a, int32_t x, int32_t y)
{
    /* The A operand is copied to the accumulator, which is apparently
       subtracted by 1 if a is positive and b is negative. */
    dev->dsp.acc = a - ((a >= 0) && (b < 0));
    return (a & x) ^ y;
}

static int32_t
emu10k1_dsp_opTSTNEG(emu10k1_t *dev, int64_t a, int32_t x, int32_t y)
{
    /* For the 3 test opcodes, hardware subtracts the operands into the accumulator and compares on that. */
    dev->dsp.acc = a - y;
    if (dev->dsp.acc < 0)
        x = ~x;
    return x;
}

static int32_t
emu10k1_dsp_opLIMIT(emu10k1_t *dev, int64_t a, int32_t x, int32_t y)
{
    dev->dsp.acc = a - y;
    return (dev->dsp.acc < 0) ? y : x;
}

static int32_t
emu10k1_dsp_opLIMIT1(emu10k1_t *dev, int64_t a, int32_t x, int32_t y)
{
    dev->dsp.acc = a - y;
    return (dev->dsp.acc < 0) ? x : y;
}

static uint32_t
emu10k1_dsp_logcompress(int32_t val, int max_exp)
{
    /* Special cases: 0 divides by 2, and 1 returns the same value. */
    if (UNLIKELY(max_exp < 2))
        return val >> (max_exp ^ 1);

    /* Based on a kX driver function written by someone smarter than me. */
    int exp_bits = log2(max_exp) + 1;
    uint32_t ret = abs(val);
    int msb = 32 - log2(ret);
    ret <<= msb;
    int exp = max_exp - msb;
    if (exp >= 0) {
        ret <<= 1;
        exp++;
    } else {
        ret >>= -1 - exp;
        exp = 0;
    }
    ret = (exp << (31 - exp_bits)) | (ret >> (exp_bits + 1));
    return (val < 0) ? -ret : ret;
}

static int32_t
emu10k1_dsp_opLOG(emu10k1_t *dev, int64_t a, int32_t x, int32_t y)
{
    /* On both LOG and EXP, the A operand is copied to the accumulator. */
    dev->dsp.acc = a;
    uint32_t r = emu10k1_dsp_logcompress(a, x & 0x1f);

    /* Apply one's complement transformations. */
    switch (y & 0x3) {
        case 0x1: if (r & 0x80000000) r = ~r; break;
        case 0x2: if (r & 0x80000000) break; /* fall-through */
        case 0x3: r = ~r; break;
    }

    return r;
}

static uint32_t
emu10k1_dsp_logdecompress(int32_t val, int max_exp)
{
    /* Special cases: 0 multiplies by 2 (and adds 1 if negative), and 1 returns the same value. */
    if (UNLIKELY(max_exp < 2))
        return (val << (max_exp ^ 1)) + ((max_exp == 0) && (val < 0));

    /* Same note as logcompress. */
    int exp_bits = log2(max_exp) + 1;
    uint32_t ret = abs(val); 
    int msb = 32 - log2(ret);
    if (msb <= exp_bits) {
        int exp = ret >> (31 - exp_bits);
        ret <<= exp_bits + 1;
        ret >>= exp_bits + 1;
        ret <<= exp_bits + 1;
        ret >>= 1;
        ret = ret + 2147483647 - 1;
        ret >>= max_exp + 1 - exp;
    } else {
        ret <<= exp_bits + 1;
        ret <<= msb - exp_bits - 1;
        ret >>= msb + max_exp - exp_bits;
    }
    return (val < 0) ? -ret : ret;
}

static int32_t
emu10k1_dsp_opEXP(emu10k1_t *dev, int64_t a, int32_t x, int32_t y)
{
    dev->dsp.acc = a;
    uint32_t r = emu10k1_dsp_logdecompress(a, x & 0x1f);

    /* Apply one's complement transformations. */
    switch (y & 0x3) {
        case 0x1: if (r & 0x80000000) r = ~r; break;
        case 0x2: if (r & 0x80000000) break; /* fall-through */
        case 0x3: r = ~r; break;
    }

    return r;
}

static int32_t
emu10k1_dsp_opINTERP(emu10k1_t *dev, int64_t a, int32_t x, int32_t y)
{
    /* Borrow flag apparently always set. */
    dev->dsp.regs[0x57] |= 0x02; /* B */
    dev->dsp.acc = emu10k1_dsp_add(dev, a, (((int64_t) x * (y - a)) >> 31));
    return emu10k1_dsp_saturate(dev, dev->dsp.acc);
}

static int32_t
emu10k1_dsp_opSKIP(emu10k1_t *dev, int64_t a, int32_t x, int32_t y)
{
    /* Generate CC bit string. */
    uint32_t cmp = a & 0x1f; /* S Z M B N */
    cmp = (cmp << 5) | (~cmp & 0x1f); /* S Z M B N S' Z' M' B' N' (hereinafter flags) */
    cmp = (cmp << 20) | (cmp << 10) | cmp; /* across 3 instances */

    /* Perform bit testing. */
    cmp = ~cmp & x; /* comparisons are inverse (example: 0x8 = zero is set, 0x100 = zero is not set) */
    uint32_t i = x & 0x3ff00000, icmp = cmp & 0x3ff00000,
             j = x & 0x000ffc00, jcmp = cmp & 0x000ffc00,
             k = x & 0x000003ff, kcmp = cmp & 0x000003ff;
    switch (x >> 30) { /* boolean equation */
        case 0x0: /* OR(AND(flags), AND(flags), AND(flags)) => only one used by open source applications... */
            cmp = (i && (icmp == i)) || (j && (jcmp == j)) || (k && (kcmp == k));
            break;

        case 0x1: /* AND(OR(flags), OR(flags), OR(flags)) => ...except this one as a magic always skip (0x7fffffff) */
            cmp = (!i || icmp) && (!j || jcmp) && (!k || kcmp);
            break;

        case 0x2: /* OR(AND(flags), AND(flags), OR(flags)) */
            cmp = (i && (icmp == i)) || (j && (jcmp == j)) || (!k || kcmp);
            break;

        case 0x3: /* AND(OR(flags), OR(flags), AND(flags)) */
            cmp = (!i || icmp) && (!j || jcmp) && (k && (kcmp == k));
            break;
    }

    /* Mark instruction skip if the test resulted in true. */
    if (cmp)
        dev->dsp.skip = y;

    return a;
}

static int32_t (*emu10k1_dsp_ops[])(emu10k1_t *dev, int64_t a, int32_t x, int32_t y) = {
    emu10k1_dsp_opMACS,
    emu10k1_dsp_opMACS1,
    emu10k1_dsp_opMACW,
    emu10k1_dsp_opMACW1,
    emu10k1_dsp_opMACINTS,
    emu10k1_dsp_opMACINTW,
    emu10k1_dsp_opACC3,
    emu10k1_dsp_opMACMV,
    emu10k1_dsp_opANDXOR,
    emu10k1_dsp_opTSTNEG,
    emu10k1_dsp_opLIMIT,
    emu10k1_dsp_opLIMIT1,
    emu10k1_dsp_opLOG,
    emu10k1_dsp_opEXP,
    emu10k1_dsp_opINTERP,
    emu10k1_dsp_opSKIP
};

/* Calculation of effective TRAM addresses (addr + DBAC) */
#define itram_addr ((tram_op + dev->dsp.regs[0x5b]) & ((sizeof(dev->dsp.itram) / sizeof(dev->dsp.itram[0])) - 1))
#define etram_addr (dev->indirect_regs[0x41] + ((tram_op + dev->dsp.regs[0x5b]) & dev->dsp.etram_mask))

static __inline uint32_t
emu10k1_dsp_read(emu10k1_t *dev, int addr)
{
    if (addr == 0x5b) /* DBAC */
        return dev->dsp.regs[0x5b] << 11; /* shifted from DSP point of view */
    else if (addr < 0x100) /* DSP registers */
        return dev->dsp.regs[addr];
    else if (addr < 0x300) /* GPR and TRAM data */
        return dev->indirect_regs[addr];
    else /* TRAM address */
        return ((dev->indirect_regs[addr] & 0x000fffff) << 11) | dev->dsp.tram_frac[addr & 0xff]; /* shifted + fractional address from DSP point of view */
}

void
emu10k1_dsp_exec(emu10k1_t *dev, int pos, int32_t *buf)
{
    /* Don't execute if the DSP is in single step mode. */
    if (dev->indirect_regs[0x52] & 0x00008000)
        return;

    if (UNLIKELY(dev->dsp.stop))
        return;

    /* Send DSP outputs from the previous run to the audio buffer.
       This should actually be 20 bits sent to the AC97 codec. */
#ifdef CREATIVE_DESCRIBED_BEHAVIOR
    buf[0] = (int16_t) ((int32_t) dev->dsp.regs[0x20] >> 14);
    buf[1] = (int16_t) ((int32_t) dev->dsp.regs[0x21] >> 14);
#else
    buf[0] = (int16_t) ((int32_t) dev->dsp.regs[0x20] >> 2);
    buf[1] = (int16_t) ((int32_t) dev->dsp.regs[0x21] >> 2);
#endif

    /* Populate FX bus inputs. */
    for (int i = 0; i < dev->emu8k.emu10k1_fxbuses; i++) {
        int32_t clip = dev->emu8k.fx_buffer[i][pos];
        if (clip < -32768)
            clip = -32768;
        else if (clip > 32767)
            clip = 32767;
        if (i < 2) buf[i] = clip;
#ifdef CREATIVE_DESCRIBED_BEHAVIOR
        dev->dsp.regs[i] = (uint16_t) clip << 14;
#else
        dev->dsp.regs[i] = (uint16_t) clip;
#endif
    }

#define RUNNING_CODE() (fetch)
#define ANY_REG(v) ((r == (v)) || (a == (v)) || (x == (v)) || (y == (v)))
#define ANY_REG_VAL(v) ((rval == (v)) || (aval == (v)) || (xval == (v)) || (yval == (v)))
//#define EMU10K1_DSP_TRACE dev->dsp.regs[4] && RUNNING_CODE() && (ANY_REG(0x04) || ANY_REG(0x20) || ANY_REG(0x102) || ANY_REG(0x10c) || ANY_REG(0x114))
//#ifdef EMU10K1_DSP_TRACE
    if (dev->dsp.regs[0])
        pclog("EMU10K1: DSP out %08" PRIX32 " %08" PRIX32 " in %08" PRIX32 " %08" PRIX32 " %08" PRIX32 " %08" PRIX32 "\n", buf[0], buf[1], dev->dsp.regs[0], dev->dsp.regs[1], dev->dsp.regs[4], dev->dsp.regs[5]);
//#endif

    /* Update TRAM. Unknown behaviors so far:
       - CLEAR
       - |ALIGN
       - multiple ops set
       - address alignment
       - 16-bit logarithmic compression format (IEEE binary16 assumed)
       - unaligned external TRAM base */
    int tram = 0;
    for (; tram < 0x80; tram++) {
        uint32_t tram_op = dev->indirect_regs[0x300 | tram];
        if (tram_op & 0x00800000) /* CLEAR (effect unknown) */
            dev->dsp.itram[itram_addr] = 0;
        else if (tram_op & 0x00200000) /* WRITE (effect of |ALIGN unknown) */
            dev->dsp.itram[itram_addr] = emu10k1_dsp_logcompress(dev->indirect_regs[0x200 | tram], 31);
        else if (tram_op & 0x00100000) /* READ (effect of |ALIGN unknown) */
            dev->indirect_regs[0x200 | tram] = emu10k1_dsp_logdecompress(dev->dsp.itram[itram_addr], 31);
    }
    if (dev->io_regs[0x14] & 0x00000004) { /* ignore external TRAM if LOCKTANKCACHE is set */
        for (; tram < 0xa0; tram++) {
            uint32_t tram_op = dev->indirect_regs[0x300 | tram];
            if (tram_op & 0x00800000) /* CLEAR (effect unknown) */
                mem_writew_phys(etram_addr, 0);
            else if (tram_op & 0x00200000) /* WRITE (effect of |ALIGN unknown) */
                mem_writew_phys(etram_addr, emu10k1_dsp_logcompress(dev->indirect_regs[0x200 | tram], 31));
            else if (tram_op & 0x00100000) /* READ (effect of |ALIGN unknown) */
                dev->indirect_regs[0x200 | tram] = emu10k1_dsp_logdecompress(mem_readw_phys(etram_addr), 31);
        }
    }

    /* Decrement DBAC. */
    dev->dsp.regs[0x5b] = (dev->dsp.regs[0x5b] - 1) & 0xfffff;

    /* THREAD SAFETY BARRIER */

    /* Execute DSP instruction stream. */
    uint64_t *code = (uint64_t *) &dev->indirect_regs[0x400];
    uint32_t pc = 0;
    while (pc < 0x200) {
        /* Decode instruction. */
        uint64_t fetch = code[pc];
        int y = fetch & 0x3ff;
        int x = (fetch >> 10) & 0x3ff;
        int a = (fetch >> 32) & 0x3ff;
        int r = (fetch >> 42) & 0x3ff;
        int op = (fetch >> 52) & 0xf;

        /* Read operands.
           The accumulator can only be specified as A, otherwise it
           reads as 0, except on MACMV where it always reads as 0. */
        uint64_t aval = ((a == 0x56) && (op != 0x7)) ? dev->dsp.acc : emu10k1_dsp_read(dev, a);
        uint32_t xval = emu10k1_dsp_read(dev, x), yval = emu10k1_dsp_read(dev, y);

        /* Clear flags now, as operation code may set them. */
        dev->dsp.regs[0x57] = 0;

        /* Execute operation. */
        int32_t rval = emu10k1_dsp_ops[op](dev, aval, xval, yval);

        /* Calculate remaining flags. */
        dev->dsp.regs[0x57] |=
            (((rval & emu10k1_dsp_read(dev, r)) & 0x80000000) >> 30) | /* N = 0x02 */
            ((rval < 0) << 2) | /* M = 0x04 */
            ((rval == 0) << 3); /* Z = 0x08 */

#ifdef EMU10K1_DSP_TRACE
        if (EMU10K1_DSP_TRACE)
            emu10k1_log("EMU10K1: %03X OP(%X, %03X:%08" PRIX32 ", %03X:%08" PRIX64 ", %03X:%08" PRIX32 ", %03X:%08" PRIX32 ") fl=%02" PRIX32 "\n",
                pc, op, r, rval, a, aval, x, xval, y, yval, dev->dsp.regs[0x57] & 0x1f);
#endif

        /* Set debug register.
           R/WC? writable? unknown for pretty much all of the debug register */
        uint32_t debug = (dev->indirect_regs[0x52] & ~0x01ff0000) | (dev->dsp.regs[0x57] << 9);
        if (dev->dsp.regs[0x57] & 0x10)
            debug |= 0x02000000 | (r << 16);
        dev->indirect_regs[0x52] = debug;

        /* Write result operand.
           Unknown behaviors:
           - writing to inputs
           - writing to accum (will depend if there are ops that can bypass the accum), CCR, DBAC
           - writing to unknown and unimplemented areas
           - reading from interrupt register */
        if (r < 0x20) { /* inputs */
            /* no-op */
        } else if (r == 0x5a) { /* interrupt register */
            dev->dsp.interrupt = 1;
        } else if ((r & ~0x1f) == 0x40) { /* constants and hardware registers */
            /* no-op */
        } else if (r < 0x100) { /* DSP registers */
            dev->dsp.regs[r] = rval;
        } else if (r < 0x300) { /* GPR and TRAM data */
            dev->indirect_regs[r] = rval;
        } else { /* TRAM address */
            dev->indirect_regs[r] = (dev->indirect_regs[r] & 0xfff00000) | ((rval >> 11) & 0x000fffff);
            dev->dsp.tram_frac[r & 0xff] = rval & 0x07ff;
        }

        /* Advance program counter, skipping instructions if required. */
        pc += 1 + dev->dsp.skip;
        dev->dsp.skip = 0;
    }
}

static void
emu10k1_update_irqs(emu10k1_t *dev)
{
    /* Set channel loop interrupts. */
    if (dev->emu8k.lip) {
        /* Calculate highest active channel for IPR_CHANNELNUMBER. */
        uint64_t any_ip = *((uint64_t *) &dev->indirect_regs[0x5a]) | *((uint64_t *) &dev->indirect_regs[0x68]);
        int channel = -1;
        while (any_ip) {
            any_ip >>= 1;
            channel++;
        }

        /* Does any channel still have pending interrupts? */
        if (channel > -1) {
            dev->io_regs[0x08] = (dev->io_regs[0x08] & ~0x7f) | 0x40 | channel; /* IPR_CHANNELLOOP | IPR_CHANNELNUMBER */
        } else {
            /* Clear interrupt if no channels are left. */
            dev->emu8k.lip = 0;
            dev->io_regs[0x08] &= ~0x7f; /* clear IPR_CHANNELLOOP | IPR_CHANNELNUMBER */
        }
    }

    /* Set forced interrupt flag. */
    if (dev->io_regs[0x0e] & 0x10) /* INTE_FORCEINT */
        dev->io_regs[0x0a] |= 0x40; /* IPR_FORCEINT */

    /* Raise or lower IRQ according to interrupt flags. */
    if (*((uint32_t *) &dev->io_regs[0x08])) {
        pci_set_irq(dev->slot, PCI_INTA);
        emu10k1_log("EMU10K1: Raising IRQ\n");
    } else {
        pci_clear_irq(dev->slot, PCI_INTA);
    }
}

static void
emu10k1_mpu0_irq_update(void *priv, int set)
{
    emu10k1_t *dev = (emu10k1_t *) priv;
    if ((dev->io_regs[0x0c] & 0x01) && dev->mpu[0].queue_used) /* IPR_MIDIRECVBUFEMPTY, seemingly a misnomer, gated by INTE_MIDIRXENABLE */
        dev->io_regs[0x08] |= 0x80;
    else
        dev->io_regs[0x08] &= ~0x80;
    if ((dev->io_regs[0x0c] & 0x02) && !dev->mpu[0].state.cmd_pending) /* IPR_MIDITRANSBUFEMPTY gated by INTE_MIDITXENABLE */
        dev->io_regs[0x09] |= 0x01;
    else
        dev->io_regs[0x09] &= ~0x01;
    emu10k1_update_irqs(dev);
}

static int
emu10k1_mpu0_irq_pending(void *priv)
{
    emu10k1_t *dev = (emu10k1_t *) priv;
    return *((uint16_t *) &dev->io_regs[0x08]) & 0x0180;
}

static void
emu10k1_mpu1_irq_update(void *priv, int set)
{
    emu10k1_t *dev = (emu10k1_t *) priv;
    if ((dev->io_regs[0x0e] & 0x01) && dev->mpu[1].queue_used) /* IPR_A_MIDIRECVBUFEMPTY2, seemingly a misnomer, gated by INTE_A_MIDIRXENABLE2 */
        dev->io_regs[0x0b] |= 0x08;
    else
        dev->io_regs[0x0b] &= ~0x08;
    if ((dev->io_regs[0x0e] & 0x02) && !dev->mpu[1].state.cmd_pending) /* IPR_A_MIDITRANSBUFEMPTY2 gated by INTE_A_MIDITXENABLE2 */
        dev->io_regs[0x0b] |= 0x10;
    else
        dev->io_regs[0x0b] &= ~0x10;
    emu10k1_update_irqs(dev);
}

static int
emu10k1_mpu1_irq_pending(void *priv)
{
    emu10k1_t *dev = (emu10k1_t *) priv;
    return dev->io_regs[0x0b] & 0x18;
}

static void
emu10k1_io_trap(int size, uint16_t addr, uint8_t write, uint8_t val, void *priv)
{
    emu10k1_io_trap_t *trap = (emu10k1_io_trap_t *) priv;

#ifdef ENABLE_EMU10K1_LOG
    if (write)
        emu10k1_log("EMU10K1: io_trap(%04X, %02X)\n", addr, val);
    else
        emu10k1_log("EMU10K1: io_trap(%04X)\n", addr);
#endif

    /* Set trap event data in HCFG. */
    trap->dev->io_regs[0x16] = (trap->dev->io_regs[0x16] & ~0xc0) | (write ? 0x80 : 0x00) | ((size > 1) ? 0x40 : 0x00) | 0x20;
    trap->dev->io_regs[0x17] = trap->flag | (addr & 0x1f); /* mask and comments disagree on [5:0] or [4:0] of address; whether or not unused bits are masked is unknown */

    /* Raise NMI. */
    nmi = 1;
}

static void
emu10k1_remap_traps(emu10k1_t *dev)
{
    io_trap_remap(dev->io_traps[TRAP_DMA1].trap, dev->io_regs[0x0f] & 0x08, 0x00, 16);
    io_trap_remap(dev->io_traps[TRAP_DMA2].trap, dev->io_regs[0x0f] & 0x04, 0xc0, 32);
    io_trap_remap(dev->io_traps[TRAP_PIC1].trap, dev->io_regs[0x0f] & 0x02, 0x20, 2);
    io_trap_remap(dev->io_traps[TRAP_PIC2].trap, dev->io_regs[0x0f] & 0x01, 0xa0, 2);
    io_trap_remap(dev->io_traps[TRAP_SB].trap, dev->io_regs[0x0e] & 0x80, 0x220 + (dev->io_regs[0x0f] >> 1), 16);
    io_trap_remap(dev->io_traps[TRAP_OPL].trap, dev->io_regs[0x0e] & 0x40, 0x388, 4);
    io_trap_remap(dev->io_traps[TRAP_MPU].trap, dev->io_regs[0x0e] & 0x20, 0x300 | (dev->io_regs[0x0f] & 0x30), 2);
}

static uint32_t
emu10k1_mmutranslate(emu10k1_t *dev, uint32_t page)
{
    uint32_t ptb = dev->indirect_regs[0x40],
             ptb_end = ptb + (dev->pagemask << 2);
    for (; ptb <= ptb_end; ptb += 4) {
        uint32_t pte = mem_readl_phys(ptb);
        if ((pte & dev->pagemask) == page) {
            /* EMU10K1 is notorious for its "31-bit" DMA, where pte[31:13] = addr[30:12] */
            pte = (pte >> (dev->type == EMU10K1)) & 0xfffff000;

            /* Add TLB entry. */
            if (dev->tlb[dev->tlb_pos] != EMU10K1_TLB_UNCACHED)
                dev->pages[dev->tlb[dev->tlb_pos]] = EMU10K1_MMU_UNMAPPED;
            dev->tlb[dev->tlb_pos++] = page;

            return pte;
        }
    }
    return EMU10K1_MMU_UNMAPPED;
}

static void
emu10k1_flushmmucache(emu10k1_t *dev)
{
    emu10k1_log("EMU10K1: flushmmucache()\n");

    /* Clear TLB entries. */
    for (int i = 0; i < (sizeof(dev->tlb) / sizeof(dev->tlb[0])); i++) {
        if (dev->tlb[i] != EMU10K1_TLB_UNCACHED) {
            dev->pages[dev->tlb[i]] = EMU10K1_MMU_UNMAPPED;
            dev->tlb[i] = EMU10K1_TLB_UNCACHED;
        }
    }
    dev->tlb_pos = 0;
}

static int16_t
emu10k1_mem_read(emu8k_t *emu8k, uint32_t addr)
{
    emu10k1_t *dev = (emu10k1_t *) emu8k;

    addr <<= 1;
    uint32_t page = addr >> 12;
    if (dev->pages[page] == EMU10K1_MMU_UNMAPPED) {
        if ((dev->pages[page] = emu10k1_mmutranslate(dev, page)) == EMU10K1_MMU_UNMAPPED)
            return 0;
    }
    return mem_readw_phys(dev->pages[page] | (addr & 0x00000fff));
}

static void
emu10k1_mem_write(emu8k_t *emu8k, uint32_t addr, uint16_t val)
{
    emu10k1_t *dev = (emu10k1_t *) emu8k;

    addr <<= 1;
    uint32_t page = addr >> 12;
    if (dev->pages[page] == EMU10K1_MMU_UNMAPPED) {
        if ((dev->pages[page] = emu10k1_mmutranslate(dev, page)) == EMU10K1_MMU_UNMAPPED)
            return;
    }
    mem_writew_phys(dev->pages[page] | (addr & 0x00000fff), val);
}

static uint8_t
emu10k1_readb(uint16_t addr, void *priv)
{
    emu10k1_t *dev = (emu10k1_t *) priv;
    addr &= 0x1f;
    uint8_t ret;
    int reg;
#ifdef ENABLE_EMU10K1_LOG
    reg = -1;
#endif

    switch (addr) {
        case 0x04 ... 0x07: /* DATA */
            reg = *((uint16_t *) &dev->io_regs[0x02]);
            switch (reg) {
                case 0x70 ... 0x73: /* A_MUDATA1 ... A_MUCMD2 */
                    if ((dev->type != EMU10K1) && !(addr & 3))
                        ret = mpu401_read(reg, &dev->mpu[(reg & 2) >> 1]);
                    else
                        ret = 0;
                    break;

                default:
                    goto readb_fallback;
            }
            break;

        case 0x18: /* MUDATA */
        case 0x19: /* MUSTAT */
            if (dev->type == EMU10K1) 
                ret = mpu401_read(addr, &dev->mpu[0]);
            else
                goto io_reg;
            break;

        case 0x10: /* 16/32-bit registers */
        case 0x1c ... 0x1d:
readb_fallback:
            emu10k1_log_push();
            ret = emu10k1_readw(addr & ~0x01, priv) >> ((addr & 1) << 3);
            emu10k1_log_pop();
            break;

        default:
io_reg:
            ret = dev->io_regs[addr];
            break;
    }

#ifdef ENABLE_EMU10K1_LOG
    if (reg > -1)
        emu10k1_log("EMU10K1: read_i(%d, %03X) = %02X\n", dev->emu8k.cur_voice, reg, ret);
    else
        emu10k1_log("EMU10K1: read(%02X) = %02X\n", addr, ret);
#endif
    return ret;
}

static uint16_t
emu10k1_readw(uint16_t addr, void *priv)
{
    emu10k1_t *dev = (emu10k1_t *) priv;
    addr &= 0x1f;
    uint16_t ret;
    int reg;
#ifdef ENABLE_EMU10K1_LOG
    reg = -1;
#endif

    switch (addr) {
        case 0x04: /* DATA */
        case 0x06:
            reg = *((uint16_t *) &dev->io_regs[0x02]);
            switch (reg) {
                case 0x00 ... 0x07: /* CPF ... DSL */
                    ret = emu8k_inw(0x600 | (addr & 2), &dev->emu8k);
                    break;

                case 0x08: /* CCCA */
                    ret = emu8k_inw(0xa00 | (addr & 2), &dev->emu8k);
                    break;

                case 0x10 ... 0x17: /* ENVVOL ... LFO2VAL */
                    dev->emu8k.cur_reg = 4 | (dev->emu8k.cur_reg >> 1);
                    ret = (addr & 2) ? 0 : emu8k_inw(0xa00 | ((reg & 1) << 1), &dev->emu8k);
                    break;

                case 0x18 ... 0x1e: /* IP ... TEMPENV */
                case 0x1f: /* EMU8000 ID register (unknown whether or not it applies here!) */
                    ret = (addr & 2) ? 0 : emu8k_inw(0xe00, &dev->emu8k);
                    break;

                case 0x70 ... 0x73: /* 8-bit registers */
                    goto readw_fallback8;

                default: /* 32-bit registers */
                    goto readw_fallback32;
            }
            break;

        case 0x1c: /* AC97DATA */
            /* Codec functions discard the MSB and LSB of AC97ADDRESS. */
            ret = dev->codec ? ac97_codec_readw(dev->codec, dev->io_regs[0x1e]) : 0;
            break;

        case 0x10: /* 32-bit registers */
        case 0x12:
readw_fallback32:
            emu10k1_log_push();
            ret = emu10k1_readl(addr & ~0x03, priv) >> ((addr & 2) << 3);
            emu10k1_log_pop();
            break;

        default: /* 8-bit registers or unaligned operation */
readw_fallback8:
            emu10k1_log_push();
            ret = emu10k1_readb(addr, priv);
            ret |= emu10k1_readb(addr + 1, priv) << 8;
            emu10k1_log_pop();
            break;
    }

#ifdef ENABLE_EMU10K1_LOG
    if (reg > -1)
        emu10k1_log("EMU10K1: read_i(%d, %03X) = %04X\n", dev->emu8k.cur_voice, reg, ret);
    else
        emu10k1_log("EMU10K1: read(%02X) = %04X\n", addr, ret);
#endif
    return ret;
}

static uint32_t
emu10k1_readl(uint16_t addr, void *priv)
{
    emu10k1_t *dev = (emu10k1_t *) priv;
    addr &= 0x1f;
    uint32_t ret;
    int reg;
#ifdef ENABLE_EMU10K1_LOG
    reg = -1;
#endif

    switch (addr) {
        case 0x04: /* DATA */
            reg = *((uint16_t *) &dev->io_regs[0x02]);
            switch (reg) {
                case 0x09: /* CCR */
                    ret = dev->emu8k.voice[dev->emu8k.cur_voice].ccr;
                    break;

                case 0x0a: /* CLP */
                    ret = dev->emu8k.voice[dev->emu8k.cur_voice].clp;
                    break;

                case 0x0b: /* FXRT */
                    ret = dev->emu8k.voice[dev->emu8k.cur_voice].fxrt;
                    break;

                case 0x0c: /* MAPA */
                    ret = dev->emu8k.voice[dev->emu8k.cur_voice].mapa;
                    break;

                case 0x0d: /* MAPB */
                    ret = dev->emu8k.voice[dev->emu8k.cur_voice].mapb;
                    break;

                case 0x7d: /* A_SENDAMOUNTS */
                    if (dev->type == EMU10K1)
                        goto indirect_reg;
                    ret = dev->emu8k.voice[dev->emu8k.cur_voice].sendamounts;
                    break;

                case 0x00 ... 0x08: /* 8/16-bit registers */
                case 0x10 ... 0x1f:
                case 0x70 ... 0x73:
                    goto readl_fallback;

                case 0x30 ... 0x3f: /* kernel: "0x30-3f seem to be the same as 0x20-2f" */
                    reg &= ~0x10;
                    /* fall-through */

                default:
indirect_reg:
                    ret = dev->indirect_regs[reg];
                    break;
            }
            break;

        case 0x10: /* WC */
            /* [5:0] is channel being processed, but we service all in one go */
            ret = (dev->emu8k.wc << 6) & 0x03ffffc0;
            break;

        default: /* 8/16-bit registers or unaligned operation */
readl_fallback:
            emu10k1_log_push();
            ret = emu10k1_readw(addr, priv);
            ret |= emu10k1_readw(addr + 2, priv) << 16;
            emu10k1_log_pop();
            break;
    }

#ifdef ENABLE_EMU10K1_LOG
    if (reg > -1)
        emu10k1_log("EMU10K1: read_i(%d, %03X) = %08X\n", dev->emu8k.cur_voice, reg, ret);
    else
        emu10k1_log("EMU10K1: read(%02X) = %08X\n", addr, ret);
#endif
    return ret;
}

static void
emu10k1_writeb(uint16_t addr, uint8_t val, void *priv)
{
    emu10k1_t *dev = (emu10k1_t *) priv;
    addr &= 0x1f;
#ifdef ENABLE_EMU10K1_LOG
    if (addr >= 0x08)
        emu10k1_log("EMU10K1: write(%02X, %02X)\n", addr, val);
#endif
    int reg;

    switch (addr) {
        case 0x00: /* PTR_CHANNELNUM */
            val &= dev->emu8k.nvoices - 1;
            dev->emu8k.cur_voice = val;
            break;

        case 0x02: /* PTR_ADDRESS[7:0] */
            dev->emu8k.cur_reg = val & 7;
            break;

        case 0x03: /* PTR_ADDRESS[10:8] */
            val &= (dev->type == EMU10K1) ? 0x07 : 0x0f;
            break;

        case 0x04 ... 0x07: /* DATA */
            reg = *((uint16_t *) &dev->io_regs[0x02]);
            emu10k1_log("EMU10K1: write_i(%d, %03X, %02X)\n", dev->emu8k.cur_voice, reg, val);
            switch (reg) {
                case 0x70 ... 0x73: /* A_MUDATA1 ... A_MUCMD2 */
                    if ((dev->type != EMU10K1) && !(addr & 3))
                        mpu401_write(reg, val, &dev->mpu[(reg & 2) >> 1]);
                    break;

                default: /* 16/32-bit registers */
                    goto writeb_fallback;
            }
            return;

        case 0x08: /* IPR[7:0] */
            dev->io_regs[addr] &= ~(val & 0xc0);
            /* Clear pending interrupt flags for a channel when it's written back. */
            if (val & 0x40) {
                val &= 0x3f;
                *((uint64_t *) &dev->indirect_regs[0x5a]) &= ~(1ULL << val);
                *((uint64_t *) &dev->indirect_regs[0x68]) &= ~(1ULL << val);
            }
            emu10k1_update_irqs(dev);
            return;

        case 0x09 ... 0x0b: /* IPR[31:8] */
            dev->io_regs[addr] &= ~val;
            emu10k1_update_irqs(dev);
            return;

        case 0x0c: /* INTE[7:0] */
            dev->io_regs[addr] = val;
            emu10k1_update_irqs(dev);
            return;

        case 0x0d: /* INTE[15:8] */
            dev->io_regs[addr] = val & 0x3f;
            emu10k1_update_irqs(dev);
            return;

        case 0x0e: /* INTE[23:16] */
            dev->io_regs[addr] = val & ((dev->type == EMU10K1) ? 0xf8 : 0xfb);
            emu10k1_update_irqs(dev);
            emu10k1_remap_traps(dev);
            return;

        case 0x0f: /* INTE[31:24] */
            dev->io_regs[addr] = val;
            emu10k1_remap_traps(dev);
            return;

        case 0x15: /* HCFG[15:8] */
            val = (val & 0x1f) | (dev->io_regs[addr] & ~0x1f);
            break;

        case 0x16: /* HCFG[23:16] */
            if (val & 0x20) /* clear LEGACYINT */
                dev->io_regs[addr] &= ~0x20;
            val = (val & 0x1d) | (dev->io_regs[addr] & ~0x1d);
            break;

        case 0x17: /* HCFG[31:24] */
            val &= 0xfd;
            break;

        case 0x18: /* MUDATA / A_GPOUTPUT */
        case 0x19: /* MUCMD / A_GPINPUT */
            if (dev->type == EMU10K1) {
                mpu401_write(addr, val, &dev->mpu[0]);
                return;
            } else if (addr & 1) {
                return;
            }
            break;

        case 0x1b: /* TIMER[9:8] */
            val &= 0x03;
            /* fall-through */

        case 0x1a: /* TIMER[7:0] */
            dev->timer_interval = *((uint32_t *) &dev->io_regs[0x1a]);
            if (dev->timer_interval == 0) /* wrap-around */
                dev->timer_interval = 1024;
            break;

        case 0x1e: /* AC97ADDRESS */
            val = (val & 0x7f) | (dev->io_regs[addr] & ~0x7f);
            break;

        case 0x1c ... 0x1d: /* 16-bit registers */
writeb_fallback:
            emu10k1_log_push();
            if (!(addr & 1))
                emu10k1_writew(addr & ~0x01, val | (emu10k1_readw(addr, priv) & 0xff00), priv);
            else
                emu10k1_writew(addr & ~0x01, (val << 8) | (emu10k1_readw(addr & ~0x01, priv) & 0x00ff), priv);
            emu10k1_log_pop();
            return;

        case 0x14: /* HCFG[7:0] */
            break;

        default:
            return;
    }

    dev->io_regs[addr] = val;
}

static void
emu10k1_writew(uint16_t addr, uint16_t val, void *priv)
{
    emu10k1_t *dev = (emu10k1_t *) priv;
    addr &= 0x1f;
#ifdef ENABLE_EMU10K1_LOG
    if ((addr != 0x00) && (addr != 0x02) && (addr != 0x04) && (addr != 0x06))
        emu10k1_log("EMU10K1: write(%02X, %04X)\n", addr, val);
#endif
    int reg;

    switch (addr) {
        case 0x04: /* DATA */
        case 0x06:
            reg = *((uint16_t *) &dev->io_regs[0x02]);
            emu10k1_log("EMU10K1: write_i(%d, %03X, %04X)\n", dev->emu8k.cur_voice, reg, val);
            switch (reg) {
                case 0x00 ... 0x07: /* CPF ... DSL */
                    emu8k_outw(0x600 | (addr & 2), val, &dev->emu8k);
                    break;

                case 0x08: /* CCCA */
                    emu8k_outw(0xa00 | (addr & 2), val, &dev->emu8k);
                    break;

                case 0x10 ... 0x17: /* ENVVOL ... LFO2VAL */
                    dev->emu8k.cur_reg = 4 | (dev->emu8k.cur_reg >> 1);
                    if (!(addr & 2))
                        emu8k_outw(0xa00 | ((reg & 1) << 1), val, &dev->emu8k);
                    break;

                case 0x18 ... 0x1e: /* IP ... TEMPENV */
                case 0x1f: /* EMU8000 ID register (unknown whether or not it applies here!) */
                    if (!(addr & 2))
                        emu8k_outw(0xe00, val, &dev->emu8k);
                    break;

                case 0x70 ... 0x73: /* 8-bit registers */
                    goto writew_fallback;

                default: /* 32-bit registers */
                    if (!(addr & 2))
                        emu10k1_writel(addr & ~0x03, val | (emu10k1_readl(addr, priv) & 0xffff0000), priv);
                    else
                        emu10k1_writel(addr & ~0x03, (val << 16) | (emu10k1_readl(addr, priv) & 0x0000ffff), priv);
                    break;
            }
            return;

        case 0x1c: /* AC97DATA */
            /* Codec functions discard the MSB and LSB of AC97ADDRESS. */
            if (dev->codec) {
                ac97_codec_writew(dev->codec, dev->io_regs[0x1e], val);

                /* Update volumes. */
                ac97_codec_getattn(dev->codec, 0x02, &dev->master_vol_l, &dev->master_vol_r);
                ac97_codec_getattn(dev->codec, 0x18, &dev->pcm_vol_l, &dev->pcm_vol_r);
                ac97_codec_getattn(dev->codec, 0x12, &dev->cd_vol_l, &dev->cd_vol_r);
            }
            break;

        default: /* 8-bit registers or unaligned operation */
writew_fallback:
            emu10k1_log_push();
            emu10k1_writeb(addr, val, priv);
            emu10k1_writeb(addr + 1, val >> 8, priv);
            emu10k1_log_pop();
            break;
    }
}

static void
emu10k1_writel(uint16_t addr, uint32_t val, void *priv)
{
    emu10k1_t *dev = (emu10k1_t *) priv;
    addr &= 0x1f;
#ifdef ENABLE_EMU10K1_LOG
    if ((addr != 0x00) && (addr != 0x04))
        emu10k1_log("EMU10K1: write(%02X, %08X)\n", addr, val);
#endif
    int reg, i;

    switch (addr) {
        case 0x04:
            reg = *((uint16_t *) &dev->io_regs[0x02]);
            emu10k1_log("EMU10K1: write_i(%d, %03X, %08X)\n", dev->emu8k.cur_voice, reg, val);
            switch (reg) {
                case 0x09: /* CCR */
                    dev->emu8k.voice[dev->emu8k.cur_voice].ccr = (val & 0xfe3f0000) | (dev->emu8k.voice[dev->emu8k.cur_voice].ccr & ~0xfe3f0000);

                    /* Invalidate TLB if a cache invalidation size is set. */
                    if (val & 0xfe000000)
                        emu10k1_flushmmucache(dev);
                    return;

                case 0x0b: /* FXRT */
                    dev->emu8k.voice[dev->emu8k.cur_voice].fxrt = val & 0xffff0000;
                    for (i = 0; i < 4; i++)
                        dev->emu8k.voice[dev->emu8k.cur_voice].fx_send[i] = (val >> (16 | (i << 2))) & 0xf;
                    return;

                case 0x0c: /* MAPA */
                    dev->emu8k.voice[dev->emu8k.cur_voice].mapa = val;
                    return;

                case 0x0d: /* MAPB */
                    dev->emu8k.voice[dev->emu8k.cur_voice].mapb = val;
                    return;

                case 0x00 ... 0x08: /* 16-bit registers */
                case 0x10 ... 0x1f:
                    goto writel_fallback;

                case 0x30 ... 0x3f: /* supposedly aliases of CD */
                    reg &= ~0x10;
                    break;

                case 0x40 ... 0x41: /* PTB ... TCB */
                case 0x45 ... 0x47: /* MICBA ... FXBA */
                    val &= 0xfffff000;
                    break;

                case 0x42: /* ADCCR */
                    val &= (dev->type == EMU10K1) ? 0x0000001f : 0x0000003f;
                    break;

                case 0x43: /* FXWC */
                    /* Masking only the bits declared by kernel constants. */
                    val &= 0x00fc3003;
                    break;

                case 0x44: /* TCBS */
                    val &= 0x00000007;
                    dev->dsp.etram_mask = (16384 << val) - 1;
                    break;

                case 0x48: /* A_HWM */
                    if (dev->type == EMU10K1)
                        return;
                    break;

                case 0x49 ... 0x4b: /* MICBS ... FXBS */
                    /* Masking only the value range declared by kernel constants. */
                    val &= 0x0000001f;
                    break;

                case 0x52: /* DBG / A_SPSC */
                    if (dev->type != EMU10K1)
                        return;

                    /* Reset DBAC if requested. */
                    if (val & 0x80000000)
                        dev->dsp.regs[0x5b] = 0;

                    val &= 0x3ffffff;
                    break;

                case 0x53: /* REG53 / A_DBG */
                    if (dev->type != EMU10K1) {
                        /* Reset DBAC if requested. */
                        if (val & 0x40000000)
                            dev->dsp.regs[0x5b] = 0;
                        val &= 0x2ffe03ff;
                    }
                    break;

                case 0x54 ... 0x56: /* SPCS0 ... SPCS2 */
                    val &= 0x3fffffff;
                    break;

                case 0x58 ... 0x59: /* CLIEL ... CLIEH */
                case 0x66 ... 0x67: /* HLIEL ... HLIEH */
                    /* Clear any pending interrupts that are being disabled. */
                    dev->indirect_regs[addr + 2] &= val;
                    emu10k1_update_irqs(dev);
                    break;

                case 0x5a ... 0x5b: /* CLIPL ... CLIPH */
                case 0x68 ... 0x69: /* HLIPL ... HLIPH */
                    dev->indirect_regs[addr] &= ~val;
                    emu10k1_update_irqs(dev);
                    return;

                case 0x5e: /* SPBYPASS */
                    val &= 0x00000f0f;
                    break;

                case 0x5f: /* AC97SLOT / A_PCB */
                    if (dev->type == EMU10K1)
                        val &= 0x00000033;
                    else
                        return;
                    break;

                case 0x6a ... 0x6f: /* A_SPRI ... A_TDOF */
                case 0x74 ... 0x75: /* A_FXWC1 ... A_FXWC2 */
                case 0x77 ... 0x7b: /* A_SRT3 ... A_TTDD */
                    if (dev->type == EMU10K1)
                        return;
                    break;

                case 0x76: /* A_SPDIF_SAMPLERATE */
                    if (dev->type == EMU10K1)
                        return;
                    val &= 0xf003eee1;
                    break;

                case 0x7c: /* A_FXRT2 */
                case 0x7e: /* A_FXRT1 */
                    if (dev->type == EMU10K1)
                        return;
                    val &= 0xbf3f3f3f; /* whatever "high bit is used for filtering" means */
                    for (i = 0; i < 4; i++)
                        dev->emu8k.voice[dev->emu8k.cur_voice].fx_send[((addr & 2) << 1) | i] = (val >> (i << 3)) & 0x3f;
                    break;

                case 0x7d: /* A_SENDAMOUNTS */
                    if (dev->type == EMU10K1)
                        return;
                    dev->emu8k.voice[dev->emu8k.cur_voice].sendamounts = val;
                    break;

                case 0x100 ... 0x1ff: /* FXGPREG / A_TANKMEMCTLREG */
                    if (dev->type != EMU10K1)
                        val &= 0x1f;
                    break;

                case 0x2a0 ... 0x2ff: /* A_TANKMEMDATAREG */
                    if (dev->type == EMU10K1)
                        return;
                    /* fall-through */

                case 0x200 ... 0x29f: /* TANKMEMDATAREG */
                    val &= 0x000fffff; 
                    break;

                case 0x3a0 ... 0x3ff: /* A_TANKMEMADDRREG */
                    if (dev->type == EMU10K1)
                        return;
                    /* fall-through */

                case 0x300 ... 0x39f: /* TANKMEMADDRREG */
                    val &= 0x00ffffff;
                    break;

                case 0x400 ... 0x5ff: /* MICROCODE / A_FXGPREG */
                    if (dev->type == EMU10K1) /* unknown if DSP opcodes should be masked */
                        val &= 0x00ffffff;
                    break;

                case 0x600 ... 0x7ff: /* overlapped MICROCODE / A_MICROCODE */
                    if (dev->type == EMU10K1) /* unknown if DSP opcodes should be masked */
                        val &= 0x00ffffff;
                    else
                        val &= 0x0f7ff7ff;
                    break;

                case 0x800 ... 0x9ff: /* A_MICROCODE */
                    if (dev->type == EMU10K1)
                        return;
                    val &= 0x0f7ff7ff; /* unknown if DSP opcodes should be masked */
                    break;

                case 0x70 ... 0x73: /* 8/16-bit registers */
                    goto writel_fallback;

                case 0x20 ... 0x2f: /* CD */
                case 0x5c ... 0x5d: /* SOLEL ... SOLEH */
                    break;

                default:
                    return;
            }

            dev->indirect_regs[reg] = val;
            return;

        default: /* 8/16-bit registers or unaligned operation */
writel_fallback:
            emu10k1_log_push();
            emu10k1_writew(addr, val, priv);
            emu10k1_writew(addr + 2, val >> 16, priv);
            emu10k1_log_pop();
            break;
    }
}

static void
emu10k1_remap(emu10k1_t *dev)
{
    if (dev->io_base)
        io_removehandler(dev->io_base, 32, emu10k1_readb, emu10k1_readw, emu10k1_readl, emu10k1_writeb, emu10k1_writew, emu10k1_writel, dev);

    dev->io_base = (dev->pci_regs[0x04] & 0x01) ? ((dev->pci_regs[0x10] & 0xe0) | (dev->pci_regs[0x11] << 8)) : 0;
    emu10k1_log("EMU10K1: remap(%04X)\n", dev->io_base);

    if (dev->io_base)
        io_sethandler(dev->io_base, 32, emu10k1_readb, emu10k1_readw, emu10k1_readl, emu10k1_writeb, emu10k1_writew, emu10k1_writel, dev);
}

static uint8_t
emu10k1_pci_read(int func, int addr, void *priv)
{
    emu10k1_t *dev = (emu10k1_t *) priv;
    uint8_t    ret;

    switch (func) {
        case 0:
            ret = dev->pci_regs[addr];
            break;

        case 1:
            ret = dev->pci_game_regs[addr];
            break;

        default:
            return 0xff;
    }

    emu10k1_log("EMU10K1: pci_read(%d, %02X) = %02X\n", func, addr, ret);
    return ret;
}

static void
emu10k1_pci_write(int func, int addr, uint8_t val, void *priv)
{
    emu10k1_t *dev = (emu10k1_t *) priv;

    emu10k1_log("EMU10K1: pci_write(%d, %02X, %02X)\n", func, addr, val);

    switch (func) {
        case 0:
            switch (addr) {
                case 0x04:
                    dev->pci_regs[addr] = val & 0x05;
                    emu10k1_remap(dev);
                    return;

                case 0x05:
                    val &= 0x05;
                    break;

                case 0x10:
                    dev->pci_regs[addr] = (val & 0xe0) | (dev->pci_regs[addr] & ~0xe0);
                    emu10k1_remap(dev);
                    return;

                case 0x11:
                    dev->pci_regs[addr] = val;
                    emu10k1_remap(dev);
                    return;

                case 0x0c:
                case 0x0d:
                case 0x3c:
                    break;

                default:
                    return;
            }

            dev->pci_regs[addr] = val;
            break;

        case 1:
            switch (addr) {
                case 0x04:
                    dev->pci_game_regs[addr] = val & 0x05;
remap_gameport:
                    gameport_remap(dev->gameport, (dev->pci_game_regs[0x04] & 0x01) ? ((dev->pci_game_regs[0x10] & 0xf8) | (dev->pci_game_regs[0x11] << 8)) : 0);
                    return;

                case 0x05:
                    val &= 0x01;
                    break;

                case 0x10:
                    dev->pci_game_regs[addr] = (val & 0xf8) | (dev->pci_game_regs[addr] & ~0xf8);
                    goto remap_gameport;

                case 0x11:
                    dev->pci_game_regs[addr] = val;
                    goto remap_gameport;

                case 0x0c:
                case 0x0d:
                    break;

                default:
                    return;
            }

            dev->pci_game_regs[addr] = val;
            break;
    }
}

static void
emu10k1_poll(void *priv)
{
    emu10k1_t *dev = (emu10k1_t *) priv;

    /* Schedule next run. */
    timer_advance_u64(&dev->poll_timer, dev->timer_latch);

    /* Run EMU8000 update routine. */
    emu8k_update(&dev->emu8k);

    /* Process channel loop interrupts. */
    int do_update_irqs = dev->emu8k.lip;

    /* Advance and check sample timer. */
    if (++dev->timer_count == dev->timer_interval) {
        dev->timer_count = 0;
        if (dev->io_regs[0x0c] & 0x04) {
            dev->io_regs[0x09] |= 0x02;
            do_update_irqs = 1;
        }
    }

    /* Process DSP interrupt. */
    if (UNLIKELY(dev->dsp.interrupt)) {
        if (dev->io_regs[0x0d] & 0x10) { /* INTE_FXDSPENABLE */
            dev->io_regs[0x0a] |= 0x80; /* IPR_FXDSP */
            do_update_irqs = 1;
        }
        dev->dsp.interrupt = 0;
    }

    /* Update interrupts if requested. */
    if (do_update_irqs)
        emu10k1_update_irqs(dev);
}

static void
emu10k1_filter_cd_audio(int channel, double *buffer, void *priv)
{
    emu10k1_t *dev = (emu10k1_t *) priv;
    double      c, volume = channel ? dev->cd_vol_r : dev->cd_vol_l;

    c       = ((*buffer) * volume) / 65536.0;
    *buffer = c;
}

static void
emu10k1_get_buffer(int32_t *buffer, int len, void *priv)
{
    emu10k1_t *dev = (emu10k1_t *) priv;

    /* Run EMU8000 update routine. */
    emu8k_update(&dev->emu8k);

    /* Apply HCFG_AUDIOENABLE mute. */
    if (dev->io_regs[0x14] & 0x01) {
        /* Fill buffer. */
        for (int c = 0; c < len * 2; c += 2) {
            buffer[c] += (((dev->emu8k.buffer[c] * dev->pcm_vol_l) >> 15) * dev->master_vol_l) >> 15;
            buffer[c + 1] += (((dev->emu8k.buffer[c + 1] * dev->pcm_vol_r) >> 15) * dev->master_vol_r) >> 15;
        }
    }

    dev->emu8k.pos = 0;
}

static void
emu10k1_speed_changed(void *priv)
{
    emu10k1_t *dev = (emu10k1_t *) priv;

    dev->timer_latch = (uint64_t) ((double) TIMER_USEC * dev->emu8k.freq);
}

static void
emu10k1_reset(void *priv)
{
    emu10k1_t *dev = (emu10k1_t *) priv;

    /* Reset PCI configuration registers. */
    memset(dev->pci_regs, 0, sizeof(dev->pci_regs));
    dev->pci_regs[0x00] = 0x02;
    dev->pci_regs[0x01] = 0x11;
    *((uint16_t *) &dev->pci_regs[0x02]) = dev->type;
    dev->pci_regs[0x06] = 0x90;
    dev->pci_regs[0x07] = 0x02;
    dev->pci_regs[0x08] = 0x08; /* TODO: investigate rev codes */
    dev->pci_regs[0x0a] = 0x01;
    dev->pci_regs[0x0b] = 0x04;
    dev->pci_regs[0x0d] = 0x20;
    dev->pci_regs[0x0e] = 0x80;
    dev->pci_regs[0x10] = 0x01;
    dev->pci_regs[0x2c] = 0x02;
    dev->pci_regs[0x2d] = 0x11;
    *((uint16_t *) &dev->pci_regs[0x2e]) = dev->id;
    dev->pci_regs[0x34] = 0xdc;
    dev->pci_regs[0x3d] = 0x01;
    dev->pci_regs[0x3e] = 0x02;
    dev->pci_regs[0x3f] = 0x14;
    dev->pci_regs[0xdc] = 0x01;
    dev->pci_regs[0xde] = 0x22;
    dev->pci_regs[0xdf] = 0x06;

    memset(dev->pci_game_regs, 0, sizeof(dev->pci_game_regs));
    dev->pci_game_regs[0x00] = 0x02;
    dev->pci_game_regs[0x01] = 0x11;
    dev->pci_game_regs[0x02] = (dev->type == EMU10K1) ? 0x02 : 0x03;
    dev->pci_game_regs[0x03] = 0x70;
    dev->pci_game_regs[0x06] = 0x90;
    dev->pci_game_regs[0x07] = 0x02;
    dev->pci_game_regs[0x08] = 0x08;
    dev->pci_game_regs[0x0a] = 0x80;
    dev->pci_game_regs[0x0b] = 0x09;
    dev->pci_game_regs[0x0d] = 0x20;
    dev->pci_game_regs[0x0e] = 0x80;
    dev->pci_game_regs[0x10] = 0x01;
    dev->pci_game_regs[0x2c] = 0x02;
    dev->pci_game_regs[0x2d] = 0x11;
    dev->pci_game_regs[0x2e] = (dev->type == EMU10K1) ? 0x20 : 0x40;
    dev->pci_game_regs[0x34] = 0xdc;
    dev->pci_game_regs[0xdc] = 0x01;
    dev->pci_game_regs[0xde] = 0x22;
    dev->pci_game_regs[0xdf] = 0x06;

    /* Reset I/O space registers. */
    dev->io_regs[0x1e] = 0x80; /* AC97ADDRESS_READY - codec ready - unknown behavior */
    dev->indirect_regs[0x48] = 0x0000003f;

    /* Reset I/O mappings. */
    emu10k1_remap(dev);
    gameport_remap(dev->gameport, 0);

    /* Invalidate any existing TLB. */
    for (int i = 0; i < (sizeof(dev->pages) / sizeof(dev->pages[0])); i++)
        dev->pages[i] = EMU10K1_MMU_UNMAPPED;
    emu10k1_flushmmucache(dev);
}

static void *
emu10k1_init(const device_t *info)
{
    emu10k1_t *dev = malloc(sizeof(emu10k1_t));
    memset(dev, 0, sizeof(emu10k1_t));

    /* Set the chip type and parameters. */
    dev->id = device_get_config_int("model");
    int i;
    for (i = 0; i < (sizeof(emu10k1_models) / sizeof(emu10k1_models[0])); i++) {
        if (emu10k1_models[i].id == dev->id) {
            dev->type = emu10k1_models[i].type;
            break;
        }
    }
    if (i >= (sizeof(emu10k1_models) / sizeof(emu10k1_models[0]))) {
        fatal("EMU10K1: Unknown type selected\n");
        return NULL;
    }
    emu10k1_log("EMU10K1: init(%04X, %04X)\n", dev->type, dev->id);

    dev->pagemask = (dev->type == EMU10K1) ? 8191 : 4095;
    memcpy(&dev->dsp.regs[0x40], dsp_constants, sizeof(dsp_constants));

    /* Initialize EMU8000 synth. */
    emu8k_init_standalone(&dev->emu8k, 64, FREQ_48000);
    dev->emu8k.emu10k1_fxbuses = (dev->type == EMU10K1) ? 16 : 64;
    dev->emu8k.emu10k1_fxsends = (dev->type == EMU10K1) ? 4 : 8;
    dev->emu8k.read = emu10k1_mem_read;
    dev->emu8k.write = emu10k1_mem_write;
    dev->emu8k.clie = (uint64_t *) &dev->indirect_regs[0x58];
    dev->emu8k.clip = (uint64_t *) &dev->indirect_regs[0x5a];
    dev->emu8k.hlie = (uint64_t *) &dev->indirect_regs[0x66];
    dev->emu8k.hlip = (uint64_t *) &dev->indirect_regs[0x68];
    dev->emu8k.sole = (uint64_t *) &dev->indirect_regs[0x5c];

    /* Initialize AC97 codec. */
    ac97_codec = &dev->codec;
    ac97_codec_count = 1;
    ac97_codec_id = 0;
    if (emu10k1_models[i].codec)
        device_add(emu10k1_models[i].codec);

    /* Initialize playback timer. */
    timer_add(&dev->poll_timer, emu10k1_poll, dev, 0);
    emu10k1_speed_changed(dev);
    timer_advance_u64(&dev->poll_timer, dev->timer_latch);

    /* Initialize playback handler and CD audio filter. */
    sound_add_handler(emu10k1_get_buffer, dev);
    sound_set_cd_audio_filter(emu10k1_filter_cd_audio, dev);

    /* Initialize MPU-401. */
    mpu401_init(&dev->mpu[0], 0, 0, M_UART, device_get_config_int("receive_input"));
    mpu401_irq_attach(&dev->mpu[0], emu10k1_mpu0_irq_update, emu10k1_mpu0_irq_pending, dev);
    if (dev->type != EMU10K1) {
        mpu401_init(&dev->mpu[1], 0, 0, M_UART, 0);
        mpu401_irq_attach(&dev->mpu[1], emu10k1_mpu1_irq_update, emu10k1_mpu1_irq_pending, dev);
    }

    /* Initialize game port. */
    dev->gameport = gameport_add(&gameport_pnp_device);

    /* Initialize I/O traps. */
    dev->io_traps[TRAP_DMA1].trap = io_trap_add(emu10k1_io_trap, &dev->io_traps[TRAP_DMA1]);
    dev->io_traps[TRAP_DMA1].flag = 0xa0;
    dev->io_traps[TRAP_DMA2].trap = io_trap_add(emu10k1_io_trap, &dev->io_traps[TRAP_DMA2]);
    dev->io_traps[TRAP_DMA2].flag = 0xe0;
    dev->io_traps[TRAP_PIC1].trap = io_trap_add(emu10k1_io_trap, &dev->io_traps[TRAP_PIC1]);
    dev->io_traps[TRAP_PIC1].flag = 0x80;
    dev->io_traps[TRAP_PIC2].trap = io_trap_add(emu10k1_io_trap, &dev->io_traps[TRAP_PIC2]);
    dev->io_traps[TRAP_PIC2].flag = 0xc0;
    dev->io_traps[TRAP_SB].trap = io_trap_add(emu10k1_io_trap, &dev->io_traps[TRAP_SB]);
    dev->io_traps[TRAP_SB].flag = 0x40;
    dev->io_traps[TRAP_OPL].trap = io_trap_add(emu10k1_io_trap, &dev->io_traps[TRAP_OPL]);
    dev->io_traps[TRAP_OPL].flag = 0x60;
    dev->io_traps[TRAP_MPU].trap = io_trap_add(emu10k1_io_trap, &dev->io_traps[TRAP_MPU]);
    /* TRAP_MPU flag is 0x00 */

    /* Add PCI card. */
    dev->slot = pci_add_card(PCI_ADD_NORMAL, emu10k1_pci_read, emu10k1_pci_write, dev);

    /* Perform initial reset. */
    emu10k1_reset(dev);

    return dev;
}

static void
emu10k1_close(void *priv)
{
    emu10k1_t *dev = (emu10k1_t *) priv;

    emu10k1_log("emu10k1: close()\n");

    free(dev);
}

static const device_config_t sb_live_config[] = {
    {
        .name = "model",
        .description = "Model",
        .type = CONFIG_SELECTION,
        .default_string = "",
        .default_int = SB_LIVE_CT4760,
        .file_filter = "",
        .spinner = { 0 },
        .selection = {
            {
                .description = "CT4760 (SigmaTel STAC9721)",
                .value = SB_LIVE_CT4760
            },
            {
                .description = "CT4780 (Crystal CS4297A)",
                .value = SB_LIVE_CT4780
            },
            {
                .description = "SB0060 (SigmaTel STAC9708)",
                .value = SB_LIVE_SB0060
            },
            {
                .description = "SB0220 (SigmaTel STAC9708)",
                .value = SB_LIVE_SB0220
            },
            { .description = "" }
        }
    },
    {
        .name = "receive_input",
        .description = "Receive input (MPU-401)",
        .type = CONFIG_BINARY,
        .default_string = "",
        .default_int = 1
    },
    { .name = "", .description = "", .type = CONFIG_END }
};

const device_t sb_live_device = {
    .name          = "Sound Blaster Live",
    .internal_name = "sb_live",
    .flags         = DEVICE_PCI,
    .local         = 0,
    .init          = emu10k1_init,
    .close         = emu10k1_close,
    .reset         = emu10k1_reset,
    { .available = NULL },
    .speed_changed = emu10k1_speed_changed,
    .force_redraw  = NULL,
    .config        = sb_live_config
};
