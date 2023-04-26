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
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/mem.h>
#include <86box/pci.h>
#include <86box/gameport.h>
#include <86box/sound.h>
#include <86box/snd_emu8k.h>

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
        .codec = cs4297a_device
    }, {
        .type = EMU10K1,
        .id = SB_LIVE_CT4760,
        .codec = stac9721_device
    }, {
        .type = EMU10K1,
        .id = SB_LIVE_SB0060,
        .codec = stac9708_device
    }, {
        .type = EMU10K1,
        .id = SB_LIVE_SB0220,
        .codec = stac9708_device
    }
};

typedef struct {
    emu8k_t emu8k; /* at the beginning so we can cast back */

    int type, slot;
    uint16_t id, io_base;

    uint8_t pci_regs[256], pci_game_regs[256], io_regs[32];

    void *gameport;
    mpu_t mpu;
} emu10k1_t;

#define ENABLE_EMU10K1_LOG 1
#ifdef ENABLE_EMU10K1_LOG
int emu10k1_do_log = ENABLE_EMU10K1_LOG;

static void
emu10k1_log(const char *fmt, ...)
{
    va_list ap;

    if (emu10k1_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define emu10k1_log(fmt, ...)
#endif

static int16_t
emu10k1_mem_read(emu8k_t *emu8k, uint32_t addr)
{
    emu10k1_t *dev = (emu10k1_t *) emu8k;
    return 0;
}

static void
emu10k1_mem_write(emu8k_t *emu8k, uint32_t addr, uint16_t val)
{
    emu10k1_t *dev = (emu10k1_t *) emu8k;
}

static uint8_t
emu10k1_read(uint16_t addr, void *priv)
{
    emu10k1_t *dev = (emu10k1_t *) priv;
    addr &= 0x1f;
    uint8_t ret;

    switch (addr) {
        case 0x04 ... 0x07: /* DATA */
            emu8k->cur_voice = *((uint16_t *) &dev->io_regs[0x00]);
            int reg = *((uint16_t *) &dev->io_regs[0x02]);
            emu8k->cur_reg = reg & 7;

            uint16_t reg_val;
            switch (reg) {
                case 0x00 ... 0x07: /* CPF ... DSL */
                    reg_val = emu8k_inw(0x600 | (addr & 2), &dev->emu8k);
                    break;

                case 0x08: /* CCCA */
                    reg_val = emu8k_inw(0xa00 | (addr & 2), &dev->emu8k);
                    break;

                case 0x09: /* STOPPED HERE */
                    reg_val = dev->emu8k.voice[channel].ccr;
                    break;

                case 0x0a:
                    reg_val = dev->emu8k.voice[channel].clp;
                    break;

                case 0x0b:
                    reg_val = dev->emu8k.voice[channel].fxrt;
                    break;

                case 0x0c:
                    reg_val = dev->emu8k.voice[channel].mapa;
                    break;

                case 0x0d:
                    reg_val = dev->emu8k.voice[channel].mapb;
                    break;

                case 0x10:
                    reg_val = dev->emu8k.voice[channel].envvol;
                    break;

                case 0x11:
                    reg_val = dev->emu8k.voice[channel].atkhldv;
                    break;

                case 0x12:
                    reg_val = dev->emu8k.voice[channel].dcysusv;
                    break;

                case 0x13:
                    reg_val = dev->emu8k.voice[channel].lfo1val;
                    break;

                case 0x14:
                    reg_val = dev->emu8k.voice[channel].envval;
                    break;

                case 0x15:
                    reg_val = dev->emu8k.voice[channel].atkhld;
                    break;

                case 0x16:
                    reg_val = dev->emu8k.voice[channel].dcysus;
                    break;

                case 0x17:
                    reg_val = dev->emu8k.voice[channel].lfo2val;
                    break;

                case 0x18:
                    reg_val = dev->emu8k.voice[channel].ip;
                    break;
            }

            emu10k1_log("EMU10K1: read_indexed(%d, %08X) = %08X\n", channel, reg, reg_val);
            return reg_val >> ((addr & 3) * 8);

        case 0x18: /* MUDATA */
        case 0x19: /* MUSTAT */
            if (dev->type == EMU10K1) 
                ret = mpu401_read(addr, &dev->mpu);
            else
                goto io_reg;
            break;

        case 0x1c ... 0x1d: /* AC97DATA */
            /* Codec functions discard the MSB and LSB of AC97ADDRESS. */
            ret = ac97_codec_readw(codec, dev->io_regs[0x1e]) >> ((addr & 1) * 8);
            break;

        default:
io_reg;
            ret = dev->io_regs[addr];
            break;
    }

    emu10k1_log("EMU10K1: read(%02X) = %02X\n", addr, ret);
    return ret;
}

static void
emu10k1_write(uint16_t addr, uint8_t val, void *priv)
{
    emu10k1_t *dev = (emu10k1_t *) priv;
    addr &= 0x1f;
    emu10k1_log("EMU10K1: write(%02X, %02X)\n", addr, val);
    uint8_t i;

    switch (addr) {
        case 0x00: /* PTR_CHANNELNUM */
            val &= dev->emu8k.nvoices - 1;
            break;

        case 0x03: /* PTR_ADDRESS[10:8] */
            val &= 0x07;
            break;

        case 0x04 ... 0x07: /* DATA */
            /* all the interesting stuff will go here */
            break;

        case 0x08: /* IPR[7:0] */
            val = dev->io_regs[addr] & ~(val & 0xc0);
            if (!(val & 0x40)) /* clear CHANNELNUMBER when clearing CHANNELLOOP */
                val &= ~0x3f;
            break;

        case 0x09 ... 0x0b: /* IPR[31:8] */
            val = dev->io_regs[addr] & ~val;
            break;

        case 0x0d: /* INTE[15:8] */
            val &= 0x3f;
            break;

        case 0x0e: /* INTE[23:16] */
            val &= (dev->type == EMU10K1) ? 0xf8 : 0xfb;
            break;

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
                mpu401_write(addr, val, &dev->mpu);
                return;
            } else if (addr & 1) {
                return;
            }
            break;

        case 0x1b: /* TIMER[9:8] */
            val &= 0x03;
            break;

        case 0x1c: /* AC97DATA[7:0] */
            /* Handling the 16-bit width of AC97 registers in a slightly janky way.
               Codec functions discard the MSB and LSB of AC97ADDRESS. */
            i = dev->io_regs[0x1e];
            ac97_codec_writew(codec, i, val | (ac97_codec_readw(codec, i) & 0xff00));
            break;

        case 0x1d: /* AC97DATA[15:8] */
            /* Same as above. */
            i = dev->io_regs[0x1e];
            ac97_codec_writew(codec, i, (ac97_codec_readw(codec, i) & 0x00ff) | (val << 8));
            break;

        case 0x1e: /* AC97ADDRESS */
            val = (val & 0x7f) | (dev->io_regs[addr] & ~0x7f);
            break;

        case 0x02: /* PTR_ADDRESS[7:0] */
        case 0x0c: /* INTE[7:0] */
        case 0x14: /* HCFG[7:0] */
        case 0x1a: /* TIMER[7:0] */
            break;

        default:
            return;
    }

    dev->io_regs[addr] = val;
}

static void
emu10k1_remap(emu10k1_t *dev)
{
    if (dev->io_base)
        io_removehandler(dev->io_base, 32, emu10k1_read, NULL, NULL, emu10k1_write, NULL, NULL, dev);

    dev->io_base = (dev->pci_regs[0x04] & 0x01) ? ((dev->pci_regs[0x10] & 0xe0) | (dev->pci_regs[0x11] << 8)) : 0;
    emu10k1_log("EMU10K1: remap(%04X)\n", dev->io_base);

    if (dev->io_base)
        io_sethandler(dev->io_base, 32, emu10k1_read, NULL, NULL, emu10k1_write, NULL, NULL, dev);
}

static uint8_t
emu10k1_pci_read(int func, int addr, void *priv)
{
    emu10k1_t *dev = (emu10k1_t *) priv;
    uint8_t    ret = 0xff;

    switch (func) {
        case 0:
            ret = dev->pci_regs[addr];
            break;

        case 1:
            ret = dev->pci_game_regs[addr];
            break;
    }

    emu10k1_log("EMU10K1: pci_read(%d, %02X) = %02X\n", func, addr, ret);
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
                    val &= 0x01;
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
emu10k1_speed_changed(void *priv)
{
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
    dev->io_regs[0x1e] = 0x80; /* AC97ADDRESS_READY */

    /* Reset I/O mappings. */
    emu10k1_remap(dev);
    gameport_remap(dev->gameport, 0);
}

static void *
emu10k1_init(const device_t *info)
{
    emu10k1_t *dev = malloc(sizeof(emu10k1_t));
    memset(dev, 0, sizeof(emu10k1_t));

    /* Set the chip type and parameters. */
    dev->id = device_get_config_int("model");
    const device_t *codec = NULL;
    for (int i = 0; i < (sizeof(emu10k1_models) / sizeof(emu10k1_models[0])); i++) {
        if (emu10k1_models[i].id == dev->id) {
            dev->type = emu10k1_models[i].type;
            codec = emu10k1_models[i].codec;
        }
    }
    if (!codec)
        fatal("EMU10K1: Unknown type selected\n");
    emu10k1_log("EMU10K1: init(%04X, %04X)\n", dev->type, dev->id);

    /* Initialize EMU8000 synth. */
    emu8k_init_standalone(&dev->emu8k, 64);
    dev->emu8k.read = emu10k1_mem_read;
    dev->emu8k.write = emu10k1_mem_write;

    /* Initialize MPU-401. */
    mpu401_init(&dev->mpu, 0, 0, M_UART, device_get_config_int("receive_input"));

    /* Initialize game port. */
    dev->gameport = gameport_add(&gameport_pnp_device);

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
