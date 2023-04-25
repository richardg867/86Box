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
}

typedef struct {
    emu8k_t emu8k; /* at the beginning so we can cast back */

    int type;
    uint16_t id;

    uint8_t pci_regs[256], pci_game_regs[256];
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
    emu8k_init_standalone(&dev->emu8k);
    dev->emu8k.read = emu10k1_mem_read;
    dev->emu8k.write = emu10k1_mem_write;

    return dev;
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
    }
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
