/*
 * 86Box	A hypervisor and IBM PC system emulator that specializes in
 *		running old operating systems and software designed for IBM
 *		PC systems and compatibles from 1981 through fairly recent
 *		system designs based on the PCI bus.
 *
 *		This file is part of the 86Box distribution.
 *
 *		Virtual Function I/O PCI passthrough handler.
 *
 *
 *
 * Author:	RichardG, <richardg867@gmail.com>
 *
 *		Copyright 2021 RichardG.
 */
#define _FILE_OFFSET_BITS 64
#define _LARGEFILE64_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <linux/vfio.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include <86box/device.h>
#include <86box/i2c.h>
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/pci.h>
#include <86box/plat.h>
#include <86box/timer.h>
#include <86box/video.h>


enum {
    NVIDIA_3D0_NONE = 0,
    NVIDIA_3D0_SELECT,
    NVIDIA_3D0_WINDOW,
    NVIDIA_3D0_READ,
    NVIDIA_3D0_WRITE
};

typedef struct {
    int		fd;
    uint64_t	precalc_offset, offset, size;
    uint32_t	emulated_offset;
    uint8_t	*mmap_base, *mmap_precalc,
		type, bar_id,
		read: 1, write: 1;
    mem_mapping_t mem_mapping, mem_mapping_add[2];
    char	name[20];

    int		*irq_active;
    event_t	*irq_event;
} vfio_region_t;

typedef struct {
    int		slot, irq_pin;
    uint8_t	mem_enabled: 1, io_enabled: 1, rom_enabled: 1;

    vfio_region_t bars[6], rom, config, vga_io_lo, vga_io_hi, vga_mem;

    int		irq_eventfd, in_irq, prev_in_irq, irq_active;
    thread_t	*irq_thread;
    event_t	*irq_event;
    pc_timer_t	irq_timer;

    struct {
	struct {
		uint8_t state;
		uint32_t offset;
	} nvidia3d0;
    } quirks;
} vfio_t;


static video_timings_t timing_default = {VIDEO_PCI, 8, 16, 32,   8, 16, 32};
static int	timing_readb = 0, timing_readw = 0, timing_readl = 0,
		timing_writeb = 0, timing_writew = 0, timing_writel = 0,
		irq_pending = 0;


#define ENABLE_VFIO_LOG 1
#ifdef ENABLE_VFIO_LOG
int vfio_do_log = ENABLE_VFIO_LOG;

static void
vfio_log(const char *fmt, ...)
{
    va_list ap;

    if (vfio_do_log) {
	va_start(ap, fmt);
	pclog_ex(fmt, ap);
	va_end(ap);
    }
}

#if ENABLE_VFIO_LOG == 2
#define vfio_log_op vfio_log
#else
#define vfio_log_op(fmt, ...)
#endif
#else
#define vfio_log(fmt, ...)
#define vfio_log_op(fmt, ...)
#endif


static uint8_t	vfio_config_readb(int func, int addr, void *priv);
static void	vfio_config_writeb(int func, int addr, uint8_t val, void *priv);


#define VFIO_RW(space, length_char, addr_type, addr_slength, val_type, val_slength) \
static val_type \
vfio_ ## space ## _read ## length_char ## _fd(addr_type addr, void *priv) \
{ \
    register vfio_region_t *region = (vfio_region_t *) priv; \
    val_type ret; \
    if (pread(region->fd, &ret, sizeof(ret), region->precalc_offset + addr) != sizeof(ret)) \
	ret = -1; \
    vfio_log_op("[%08X:%04X] VFIO: " #space "_read" #length_char "_fd(%0" #addr_slength "X) = %0" #val_slength "X\n", CS, cpu_state.pc, addr, ret); \
    cycles -= timing_read ## length_char; \
    *(region->irq_active) = 0; \
    return ret; \
} \
\
static void \
vfio_ ## space ## _write ## length_char ## _fd(addr_type addr, val_type val, void *priv) \
{ \
    register vfio_region_t *region = (vfio_region_t *) priv; \
    vfio_log_op("[%08X:%04X] VFIO: " #space "_write" #length_char "_fd(%0" #addr_slength "X, %0" #val_slength "X)\n", CS, cpu_state.pc, addr, val); \
    pwrite(region->fd, &val, sizeof(val), region->precalc_offset + addr); \
    cycles -= timing_write ## length_char; \
    *(region->irq_active) = 0; \
} \
\
static val_type \
vfio_ ## space ## _read ## length_char ## _mm(addr_type addr, void *priv) \
{ \
    register vfio_region_t *region = (vfio_region_t *) priv; \
    register val_type ret = *((val_type *) &region->mmap_precalc[addr]); \
    vfio_log_op("[%08X:%04X] VFIO: " #space "_read" #length_char "_mm(%0" #addr_slength "X) = %0" #val_slength "X\n", CS, cpu_state.pc, addr, ret); \
    cycles -= timing_read ## length_char; \
    *(region->irq_active) = 0; \
    return ret; \
} \
\
static void \
vfio_ ## space ## _write ## length_char ## _mm(addr_type addr, val_type val, void *priv) \
{ \
    register vfio_region_t *region = (vfio_region_t *) priv; \
    vfio_log_op("[%08X:%04X] VFIO: " #space "_write" #length_char "_mm(%0" #addr_slength "X, %0" #val_slength "X)\n", CS, cpu_state.pc, addr, val); \
    *((val_type *) &region->mmap_precalc[addr]) = val; \
    cycles -= timing_write ## length_char; \
    *(region->irq_active) = 0; \
}

VFIO_RW(mem, b, uint32_t, 8, uint8_t, 2)
VFIO_RW(mem, w, uint32_t, 8, uint16_t, 4)
VFIO_RW(mem, l, uint32_t, 8, uint32_t, 8)
VFIO_RW(io, b, uint16_t, 4, uint8_t, 2)
VFIO_RW(io, w, uint16_t, 4, uint16_t, 4)
VFIO_RW(io, l, uint16_t, 4, uint32_t, 8)


/* These read/write functions help with porting quirks from QEMU. */
static uint32_t
vfio_config_read(int func, uint8_t addr, uint8_t size, void *priv)
{
    if (size == 2)
	addr &= 0xfe;
    else if (size == 4)
	addr &= 0xfc;

    uint32_t ret = vfio_config_readb(func, addr, priv);
    if (size >= 2) {
	ret |= vfio_config_readb(func, addr | 1, priv) << 8;
	if (size == 4) {
		ret |= vfio_config_readb(func, addr | 2, priv) << 16;
		ret |= vfio_config_readb(func, addr | 3, priv) << 24;
	}
    }
    return ret;
}


static void
vfio_config_write(int func, uint8_t addr, uint32_t val, uint8_t size, void *priv)
{
    if (size == 2)
	addr &= 0xfe;
    else if (size == 4)
	addr &= 0xfc;

    vfio_config_writeb(func, addr, val, priv);
    if (size >= 2) {
	vfio_config_writeb(func, addr | 1, val >> 8, priv);
	if (size == 4) {
		vfio_config_writeb(func, addr | 2, val >> 16, priv);
		vfio_config_writeb(func, addr | 3, val >> 24, priv);
	}
    }
}


static uint32_t
vfio_io_reads_fd(uint16_t addr, uint8_t size, void *priv)
{
    if (size == 1)
	return vfio_io_readb_fd(addr, priv);
    else if (size == 2)
	return vfio_io_readw_fd(addr, priv);
    else
	return vfio_io_readl_fd(addr, priv);
}


static uint32_t
vfio_io_writes_fd(uint16_t addr, uint32_t val, uint8_t size, void *priv)
{
    if (size == 1)
	vfio_io_writeb_fd(addr, val, priv);
    else if (size == 2)
	vfio_io_writew_fd(addr, val, priv);
    else
	vfio_io_writel_fd(addr, val, priv);
}


#define VFIO_RW_BWL(name, addr_type) \
static uint8_t \
vfio_ ## name ## _readb(addr_type addr, void *priv) \
{ \
    return vfio_ ## name ## _read(addr, 1, priv); \
} \
\
static uint16_t \
vfio_ ## name ## _readw(addr_type addr, void *priv) \
{ \
    return vfio_ ## name ## _read(addr, 2, priv); \
} \
\
static uint32_t \
vfio_ ## name ## _readl(addr_type addr, void *priv) \
{ \
    return vfio_ ## name ## _read(addr, 4, priv); \
} \
\
static void \
vfio_ ## name ## _writeb(addr_type addr, uint8_t val, void *priv) \
{ \
    vfio_ ## name ## _write(addr, val, 1, priv); \
} \
\
static void \
vfio_ ## name ## _writew(addr_type addr, uint16_t val, void *priv) \
{ \
    vfio_ ## name ## _write(addr, val, 2, priv); \
} \
\
static void \
vfio_ ## name ## _writel(addr_type addr, uint32_t val, void *priv) \
{ \
    vfio_ ## name ## _write(addr, val, 4, priv); \
}
/* End helper functions. */


static uint32_t
vfio_quirk_configmirror_read(uint32_t addr, uint8_t size, void *priv)
{
    vfio_t *dev = (vfio_t *) priv;

    /* Read configuration register. */
    return vfio_config_read(0, addr, size, dev);
}


static void
vfio_quirk_configmirror_write(uint32_t addr, uint32_t val, uint8_t size, void *priv)
{
    vfio_t *dev = (vfio_t *) priv;

    /* Write configuration register. */
    vfio_config_write(0, addr, val, size, dev);
}


VFIO_RW_BWL(quirk_configmirror, uint32_t);


static uint32_t
vfio_quirk_nvidia3d0_read(uint16_t addr, uint8_t size, void *priv)
{
    vfio_t *dev = (vfio_t *) priv;

    /* Cascade to the main handler. */
    uint8_t prev_state = dev->quirks.nvidia3d0.state;
    uint32_t ret = vfio_io_reads_fd(addr, size, &dev->vga_io_hi);
    dev->quirks.nvidia3d0.state = NVIDIA_3D0_NONE;

    /* Interpret NVIDIA commands. */
    if ((addr < 0x3d4) && (prev_state == NVIDIA_3D0_READ) &&
	((dev->quirks.nvidia3d0.offset & 0xffffff00) == 0x00001800)) {
	/* Configuration read. */
	ret = vfio_config_read(0, dev->quirks.nvidia3d0.offset, size, dev);
	vfio_log("VFIO: NVIDIA 3D0: Read %08X from %08X\n", ret, dev->quirks.nvidia3d0.offset & 0xff);
    }

    return ret;
}


static void
vfio_quirk_nvidia3d0_write(uint16_t addr, uint32_t val, uint8_t size, void *priv)
{
    vfio_t *dev = (vfio_t *) priv;

    uint8_t prev_state = dev->quirks.nvidia3d0.state,
	    offset;
    dev->quirks.nvidia3d0.state = NVIDIA_3D0_NONE;

    /* Interpret NVIDIA commands. */
    if (addr < 0x3d4) {
	if (prev_state == NVIDIA_3D0_SELECT) {
		/* Offset write. */
		dev->quirks.nvidia3d0.offset = val;
		dev->quirks.nvidia3d0.state = NVIDIA_3D0_WINDOW;
	} else if (prev_state == NVIDIA_3D0_WRITE) {
		if ((dev->quirks.nvidia3d0.offset & 0xffffff00) == 0x00001800) {
			/* Configuration write. */
			vfio_log("VFIO: NVIDIA 3D0: Write %08X to %08X\n", val, dev->quirks.nvidia3d0.offset & 0xff);
			vfio_config_write(0, dev->quirks.nvidia3d0.offset, val, size, dev);
			return;
		}
	}
    } else {
	switch (val) {
		case 0x338:
			if (prev_state == NVIDIA_3D0_NONE)
				dev->quirks.nvidia3d0.state = NVIDIA_3D0_SELECT;
			break;

		case 0x538:
			if (prev_state == NVIDIA_3D0_WINDOW)
				dev->quirks.nvidia3d0.state = NVIDIA_3D0_READ;
			break;

		case 0x738:
			if (prev_state == NVIDIA_3D0_WINDOW)
				dev->quirks.nvidia3d0.state = NVIDIA_3D0_WRITE;
			break;
	}
    }

    /* Cascade to the main handler. */
    vfio_io_writes_fd(addr, val, size, &dev->vga_io_hi);
}


VFIO_RW_BWL(quirk_nvidia3d0, uint16_t);


static void
vfio_quirk_configmirror(vfio_t *dev, vfio_region_t *bar, uint32_t offset, uint8_t mapping_slot, uint8_t enable)
{
    /* Get the additional memory mapping structure. */
    mem_mapping_t *mapping = &bar->mem_mapping_add[mapping_slot];

    vfio_log("VFIO: %sapping configuration space mirror for %s @ %08X\n",
	     enable ? "M" : "Unm", bar->name, offset);

    /* Add mapping if it wasn't already added.
       Being added afterwards, it should override the main mapping. */
    if (!mapping->base)
	mem_mapping_add(mapping, offset, 0,
			vfio_quirk_configmirror_readb,
			vfio_quirk_configmirror_readw,
			vfio_quirk_configmirror_readl,
			vfio_quirk_configmirror_writeb,
			vfio_quirk_configmirror_writew,
			vfio_quirk_configmirror_writel,
			NULL, MEM_MAPPING_EXTERNAL, dev);

    /* Enable or disable mapping. */
    if (enable)
	mem_mapping_set_addr(mapping, offset, 256);
    else
	mem_mapping_disable(mapping);
}


static void
vfio_quirk_remap(vfio_t *dev, vfio_region_t *bar, uint8_t enable)
{
    /* Read vendor ID. */
    uint16_t vendor;
    if (pread(dev->config.fd, &vendor, sizeof(vendor), dev->config.offset) != sizeof(vendor))
	vendor = 0x0000;

    if ((vendor == 0x1002) && (bar->size == 32) &&
	(dev->bars[4].type == 0x01) && (dev->bars[4].size >= 256)) {
	vfio_log("VFIO: %sapping ATI 3C3 quirk\n", enable ? "M" : "Unm");


    } else if (vendor == 0x10de) {
	/* BAR 0 configuration space mirrors. */
	if (bar->bar_id == 0) {
		vfio_quirk_configmirror(dev, bar, 0x1800, 0, enable);
		vfio_quirk_configmirror(dev, bar, 0x88000, 1, enable);
	}

	/* Port 3D0 configuration space mirror. */
	if ((bar->bar_id == 0xfe) && (bar->size == 32) && dev->bars[1].size) {
		vfio_log("VFIO: %sapping NVIDIA 3D0 quirk\n", enable ? "M" : "Unm");

		/* Remove quirk handler from port range. */
		io_removehandler(0x3d0, 8,
				 bar->read ? vfio_quirk_nvidia3d0_readb : NULL,
				 bar->read ? vfio_quirk_nvidia3d0_readw : NULL,
				 bar->read ? vfio_quirk_nvidia3d0_readl : NULL,
				 bar->write ? vfio_quirk_nvidia3d0_writeb : NULL,
				 bar->write ? vfio_quirk_nvidia3d0_writew : NULL,
				 bar->write ? vfio_quirk_nvidia3d0_writel : NULL,
				 dev);

		if (enable) {
			/* Remove existing handler from port range. */
			if (bar->mmap_base) /* mmap available */
				io_removehandler(0x3d0, 8,
						 bar->read ? vfio_io_readb_mm : NULL,
						 bar->read ? vfio_io_readw_mm : NULL,
						 bar->read ? vfio_io_readl_mm : NULL,
						 bar->write ? vfio_io_writeb_mm : NULL,
						 bar->write ? vfio_io_writew_mm : NULL,
						 bar->write ? vfio_io_writel_mm : NULL,
						 bar);
			else /* mmap not available */
				io_removehandler(0x3d0, 8,
						 bar->read ? vfio_io_readb_fd : NULL,
						 bar->read ? vfio_io_readw_fd : NULL,
						 bar->read ? vfio_io_readl_fd : NULL,
						 bar->write ? vfio_io_writeb_fd : NULL,
						 bar->write ? vfio_io_writew_fd : NULL,
						 bar->write ? vfio_io_writel_fd : NULL,
						 bar);

			/* Add quirk handler to port range. */
			io_sethandler(0x3d0, 8,
				      bar->read ? vfio_quirk_nvidia3d0_readb : NULL,
				      bar->read ? vfio_quirk_nvidia3d0_readw : NULL,
				      bar->read ? vfio_quirk_nvidia3d0_readl : NULL,
				      bar->write ? vfio_quirk_nvidia3d0_writeb : NULL,
				      bar->write ? vfio_quirk_nvidia3d0_writew : NULL,
				      bar->write ? vfio_quirk_nvidia3d0_writel : NULL,
				      dev);
		}
	}
    }
}


static uint8_t
vfio_bar_gettype(vfio_t *dev, vfio_region_t *bar)
{
    /* Read BAR type from device if unknown. */
    if (bar->type == 0xff) {
	if (pread(dev->config.fd, &bar->type, 1, dev->config.offset + 0x10 + (bar->bar_id << 2)) == 1)
		bar->type &= 0x01;
	else
		bar->type = 0xff;
    }

    /* Return stored BAR type. */
    return bar->type;
}


static void
vfio_bar_remap(vfio_t *dev, vfio_region_t *bar, uint32_t new_offset)
{
    vfio_log("VFIO: bar_remap(%s, %08X)\n", bar->name, new_offset);

    /* Act according to the BAR type. */
    uint8_t bar_type = vfio_bar_gettype(dev, bar);
    uint16_t vga_bitmap;
    if (bar_type == 0x00) { /* Memory BAR */
	if (bar->emulated_offset) {
		vfio_log("VFIO: Unmapping %s memory @ %08X-%08X\n", bar->name,
			 bar->emulated_offset, bar->emulated_offset + bar->size - 1);

		/* Unmap any quirks. */
		vfio_quirk_remap(dev, bar, 0);

		/* Disable memory mapping. */
		mem_mapping_disable(&bar->mem_mapping);
	}
	if (((bar->bar_id == 0xff) ? dev->rom_enabled : dev->mem_enabled) && new_offset) {
		vfio_log("VFIO: Mapping %s memory @ %08X-%08X\n",
			 bar->name, new_offset, new_offset + bar->size - 1);

		/* Enable memory mapping. */
		mem_mapping_set_addr(&bar->mem_mapping, new_offset, bar->size);

		/* Map any quirks. */
		vfio_quirk_remap(dev, bar, 1);
	}
    } else if (bar_type == 0x01) { /* I/O BAR */
	if (bar->emulated_offset) {
		vfio_log("VFIO: Unmapping %s I/O @ %04X-%04X\n", bar->name,
			 bar->emulated_offset, bar->emulated_offset + bar->size - 1);

		/* Unmap any quirks. */
		vfio_quirk_remap(dev, bar, 0);

		/* Disable I/O mapping. */
		if (bar->mmap_base) /* mmap available */
			io_removehandler(bar->emulated_offset, bar->size,
					 bar->read ? vfio_io_readb_mm : NULL,
					 bar->read ? vfio_io_readw_mm : NULL,
					 bar->read ? vfio_io_readl_mm : NULL,
					 bar->write ? vfio_io_writeb_mm : NULL,
					 bar->write ? vfio_io_writew_mm : NULL,
					 bar->write ? vfio_io_writel_mm : NULL,
					 bar);
		else /* mmap not available */
			io_removehandler(bar->emulated_offset, bar->size,
					 bar->read ? vfio_io_readb_fd : NULL,
					 bar->read ? vfio_io_readw_fd : NULL,
					 bar->read ? vfio_io_readl_fd : NULL,
					 bar->write ? vfio_io_writeb_fd : NULL,
					 bar->write ? vfio_io_writew_fd : NULL,
					 bar->write ? vfio_io_writel_fd : NULL,
					 bar);
	}
	if (dev->io_enabled && new_offset) {
		vfio_log("VFIO: Mapping %s I/O @ %04X-%04X\n", bar->name,
			 new_offset, new_offset + bar->size - 1);

		/* Enable I/O mapping. */
		if (bar->mmap_base) /* mmap available */
			io_sethandler(new_offset, bar->size,
				      bar->read ? vfio_io_readb_mm : NULL,
				      bar->read ? vfio_io_readw_mm : NULL,
				      bar->read ? vfio_io_readl_mm : NULL,
				      bar->write ? vfio_io_writeb_mm : NULL,
				      bar->write ? vfio_io_writew_mm : NULL,
				      bar->write ? vfio_io_writel_mm : NULL,
				      bar);
		else /* mmap not available */
			io_sethandler(new_offset, bar->size,
				      bar->read ? vfio_io_readb_fd : NULL,
				      bar->read ? vfio_io_readw_fd : NULL,
				      bar->read ? vfio_io_readl_fd : NULL,
				      bar->write ? vfio_io_writeb_fd : NULL,
				      bar->write ? vfio_io_writew_fd : NULL,
				      bar->write ? vfio_io_writel_fd : NULL,
				      bar);

		/* Map any quirks. */
		vfio_quirk_remap(dev, bar, 1);
	}
    }

    /* Set new emulated and precalculated offsets.
       The precalculated offsets speed up read/write operations. */
    bar->emulated_offset = new_offset;
    bar->precalc_offset = bar->offset - new_offset;
    bar->mmap_precalc = bar->mmap_base - new_offset;
}


static uint8_t
vfio_config_readb(int func, int addr, void *priv)
{
    vfio_t *dev = (vfio_t *) priv;
    if (func)
	return 0xff;

    dev->irq_active = 0;

    /* Read register from device. */
    uint8_t ret;
    if (pread(dev->config.fd, &ret, 1, dev->config.offset + addr) != 1) {
	vfio_log("VFIO: config_read(%d, %02X) failed\n", func, addr);
	return 0xff;
    }

    /* Change value accordingly. */
    uint8_t bar_id, offset, new;
    switch (addr) {
	case 0x10 ... 0x27: /* BARs */
		/* Stop if this BAR is absent. */
		bar_id = (addr - 0x10) >> 2;
		if (!dev->bars[bar_id].read && !dev->bars[bar_id].write) {
			ret = 0x00;
			break;
		}

		/* Mask off and insert static bits. */
		offset = (addr & 0x03) << 3;
		new = dev->bars[bar_id].emulated_offset >> offset;
		if (!offset) {
			switch (vfio_bar_gettype(dev, &dev->bars[bar_id])) {
				case 0x00: /* Memory BAR */
					new = (new & ~0x07) | (ret & 0x07);
					break;

				case 0x01: /* I/O BAR */
					new = (new & ~0x03) | (ret & 0x03);
					break;
			}
		}
		ret = new;
		break;

	case 0x30 ... 0x33: /* Expansion ROM */
		/* Stop if the ROM is absent. */
		if (!dev->rom.read && !dev->rom.write) {
			ret = 0x00;
			break;
		}

		/* Mask off and insert ROM enable bit. */
		offset = (addr & 0x03) << 3;
		ret = dev->rom.emulated_offset >> offset;
		if (!offset)
			ret = (ret & ~0x01) | dev->rom_enabled;
		break;
    }

    vfio_log("VFIO: config_read(%02X) = %02X\n", addr, ret);

    return ret;
}


static void
vfio_config_writeb(int func, int addr, uint8_t val, void *priv)
{
    vfio_t *dev = (vfio_t *) priv;
    if (func)
	return;

    vfio_log("VFIO: config_write(%02X, %02X)\n", addr, val);

    dev->irq_active = 0;

    /* VFIO should block anything we shouldn't write to, such as BARs. */
    pwrite(dev->config.fd, &val, 1, dev->config.offset + addr);

    /* Act on some written values. */
    uint8_t bar_id, offset;
    uint32_t new_offset;
    int i;
    switch (addr) {
	case 0x04: /* Command */
		/* Set Memory and I/O flags. */
		dev->mem_enabled = !!(val & PCI_COMMAND_MEM);
		dev->io_enabled = !!(val & PCI_COMMAND_IO);

		vfio_log("VFIO: Command Memory[%d] I/O[%d]\n", dev->mem_enabled, dev->io_enabled);

		/* Remap all BARs. */
		for (i = 0; i < 6; i++)
			vfio_bar_remap(dev, &dev->bars[i], dev->bars[i].emulated_offset);

		/* Remap VGA region. */
		vfio_bar_remap(dev, &dev->vga_io_lo, 0x3b0);
		vfio_bar_remap(dev, &dev->vga_io_hi, 0x3c0);
		vfio_bar_remap(dev, &dev->vga_mem, 0xa0000);
		break;

	case 0x10 ... 0x27: /* BARs */
		/* Stop if this BAR is absent. */
		bar_id = (addr - 0x10) >> 2;
		if (!dev->bars[bar_id].read && !dev->bars[bar_id].write)
			break;

		/* Mask off static bits. */
		offset = (addr & 0x03) << 3;
		if (!offset) {
			switch (vfio_bar_gettype(dev, &dev->bars[bar_id])) {
				case 0x00: /* Memory BAR */
					val &= ~0x07;
					break;

				case 0x01: /* I/O BAR */
					val &= ~0x03;
					break;
			}
		}

		/* Remap BAR. */
		new_offset = dev->bars[bar_id].emulated_offset & ~(0x000000ff << offset);
		new_offset |= val << offset;
		new_offset &= ~((1 << log2i(dev->bars[bar_id].size)) - 1);
		vfio_bar_remap(dev, &dev->bars[bar_id], new_offset);
		break;

	case 0x30 ... 0x33: /* Expansion ROM */
		/* Stop if the ROM is absent. */
		if (!dev->rom.read && !dev->rom.write)
			break;

		/* Set ROM enable bit. */
		offset = (addr & 0x03) << 3;
		if (!offset) {
			dev->rom_enabled = val & 0x01;
			val &= 0xfe;
		}

		/* Remap ROM. */
		new_offset = (dev->rom.emulated_offset & ~(0x000000ff << offset));
		new_offset |= (val << offset);
		new_offset &= ~((1 << log2i(dev->rom.size)) - 1);
		vfio_bar_remap(dev, &dev->rom, new_offset);
		break;
    }
}


static void
vfio_irq_thread(void *priv)
{
    vfio_t *dev = (vfio_t *) priv;
    uint64_t buf;
    struct vfio_irq_set irq_set = {
	.argsz = sizeof(irq_set),
	.flags = 0,
	.index = VFIO_PCI_INTX_IRQ_INDEX,
	.start = 0,
	.count = 1
    };
    int device = dev->config.fd;

    while (1) {
	/* Unmask host IRQ. */
	irq_set.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_UNMASK;
	ioctl(device, VFIO_DEVICE_SET_IRQS, &irq_set);

	/* Wait for an interrupt to come in. */
	vfio_log_op("VFIO: Waiting for IRQ...\n");
	read(dev->irq_eventfd, &buf, sizeof(buf));
	vfio_log_op("VFIO: IRQ has arrived: %08lX\n", buf);

	/* Mask host IRQ. */
	irq_set.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_MASK;
	ioctl(device, VFIO_DEVICE_SET_IRQS, &irq_set);

	/* Tell the timer to raise the IRQ. */
	dev->in_irq = 1;

	/* Wait for a BAR read/write to lower the IRQ. */
	thread_wait_event(dev->irq_event, -1);
	thread_reset_event(dev->irq_event);
    }
}


static void
vfio_irq_timer(void *priv)
{
    vfio_t *dev = (vfio_t *) priv;

    /* Schedule next run. */
    timer_advance_u64(&dev->irq_timer, TIMER_USEC * 100);

    /* Stop if we're not in an IRQ at the moment. */
    if (!dev->in_irq)
	return;

    /* Process an IRQ status change. */
    if (!dev->prev_in_irq) { /* rising edge */
	vfio_log_op("VFIO: Raising IRQ on pin %c\n", '@' + dev->irq_pin);

	/* Raise IRQ. */
	pci_set_irq(dev->slot, dev->irq_pin);

	/* Mark the IRQ as active, so that a BAR read/write can lower it. */
	dev->prev_in_irq = dev->irq_active = 1;
    } else if (!dev->irq_active) { /* falling edge */
	vfio_log_op("VFIO: Lowering IRQ on pin %c\n", '@' + dev->irq_pin);

	/* Lower IRQ. */
	pci_clear_irq(dev->slot, dev->irq_pin);

	/* Mark the IRQ as no longer active. */
	dev->prev_in_irq = dev->irq_active = dev->in_irq = 0;

	/* Unblock the IRQ thread. */
	thread_set_event(dev->irq_event);
    }
}


static void
vfio_prepare_region(vfio_t *dev, int device, struct vfio_region_info *reg, vfio_region_t *region)
{
    /* Set region structure information. */
    region->fd = device;
    region->offset = reg->offset;
    if (reg->index == VFIO_PCI_VGA_REGION_INDEX) {
	region->bar_id = 0xfe;
	if (region == &dev->vga_io_lo) {
		region->offset += 0x3b0;
		region->size = 12;
		region->type = 0x01;
	} else if (region == &dev->vga_io_hi) {
		region->offset += 0x3c0;
		region->size = 32;
		region->type = 0x01;
	} else {
		region->offset += 0xa0000;
		region->size = 131072;
		region->type = 0x00;
	}
    } else {
	region->size = reg->size;
	region->type = 0xff;
    }
    region->read = !!(reg->flags & VFIO_REGION_INFO_FLAG_READ);
    region->write = !!(reg->flags & VFIO_REGION_INFO_FLAG_WRITE);
    region->irq_active = &dev->irq_active;
    region->irq_event = dev->irq_event;

    vfio_log("VFIO: Region: %s (offset %lX) (%d bytes)", region->name, region->offset, region->size);

    /* Use special memory mapping for expansion ROMs. */
    if (reg->index == VFIO_PCI_ROM_REGION_INDEX) {
	/* Set specific information. */
	region->type = 0x00;
	region->bar_id = 0xff;

	/* Allocate ROM shadow area. */
	region->mmap_base = region->mmap_precalc = malloc(region->size);
	if (!region->mmap_base)
		fatal("\nVFIO: malloc(%d) failed\n", region->size);
	memset(region->mmap_base, 0xff, region->size);

	int i, j = 0;
	if (1) {
		/* Read ROM from file. */		
		FILE *f = fopen("/mnt/pool1/Users/Richard/FX5500.ROM", "rb");
		while ((i = fread(region->mmap_precalc, 1,
				  region->size - j,
				  f)) != 0) {
			region->mmap_precalc += i;
			j += i;
		}
		fclose(f);

		vfio_log(" (%d file bytes)", j);
	} else {
		/* Read ROM from device. */
		while ((i = pread(device, region->mmap_precalc,
				  region->size - j,
				  region->offset + j)) != 0) {
			region->mmap_precalc += i;
			j += i;
		}

		vfio_log(" (%d actual bytes)", j);
	}
    } else {
	/* Attempt to mmap the region. */
	region->mmap_base = mmap(NULL, region->size,
				 (region->read ? PROT_READ : 0) |
				 (region->write ? PROT_WRITE : 0),
				 MAP_SHARED, device, region->offset);
	if (region->mmap_base == ((void *) -1)) /* mmap failed */
		region->mmap_base = NULL;
    }
    region->mmap_precalc = region->mmap_base;

    /* Create memory mapping for if we need it. */
    if (region->mmap_base) { /* mmap available */
	vfio_log(" (MM)");
	mem_mapping_add(&region->mem_mapping, 0, 0,
			region->read ? vfio_mem_readb_mm : NULL,
			region->read ? vfio_mem_readw_mm : NULL,
			region->read ? vfio_mem_readl_mm : NULL,
			region->write ? vfio_mem_writeb_mm : NULL,
			region->write ? vfio_mem_writew_mm : NULL,
			region->write ? vfio_mem_writel_mm : NULL,
			NULL, MEM_MAPPING_EXTERNAL, region);
    } else { /* mmap not available */
	vfio_log(" (FD)");
	mem_mapping_add(&region->mem_mapping, 0, 0,
			region->read ? vfio_mem_readb_fd : NULL,
			region->read ? vfio_mem_readw_fd : NULL,
			region->read ? vfio_mem_readl_fd : NULL,
			region->write ? vfio_mem_writeb_fd : NULL,
			region->write ? vfio_mem_writew_fd : NULL,
			region->write ? vfio_mem_writel_fd : NULL,
			NULL, MEM_MAPPING_EXTERNAL, region);
    }

    vfio_log(" (%c%c)\n", region->read ? 'R' : '-', region->write ? 'W' : '-');
}


static void
vfio_map_dma(int container, void *ptr, uint32_t offset, uint32_t size)
{
    struct vfio_iommu_type1_dma_map dma_map = {
	.argsz = sizeof(dma_map),
	.vaddr = (uint64_t) (uintptr_t) ptr,
	.iova = offset,
	.size = size,
	.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE
    };

    vfio_log("VFIO: map_dma(%lX, %08X, %d)\n", ptr, offset, size);

    /* Map DMA region. */
    if (!ioctl(container, VFIO_IOMMU_MAP_DMA, &dma_map))
	return;

    /* QEMU says the mapping should be retried in case of EBUSY. */
    if (errno == EBUSY) {
	ioctl(container, VFIO_IOMMU_UNMAP_DMA, &dma_map);
	if (!ioctl(container, VFIO_IOMMU_MAP_DMA, &dma_map))
		return;
    }

    fatal("VFIO: map_dma(%lX, %08X, %d) failed (%d)\n", ptr, offset, size, errno);
}


static void
vfio_reset(void *priv)
{
    vfio_t *dev = (vfio_t *) priv;
    if (!dev)
	return;

    /* Unmap BAR and VGA regions. */
    vfio_config_writeb(0, 0x04, vfio_config_readb(0, 0x04, dev) & ~0x07, dev);

    /* Disable INTx. */
    vfio_config_writeb(0, 0x05, vfio_config_readb(0, 0x05, dev) & ~0x04, dev);

    /* Unmap expansion ROM region. */
    dev->rom.emulated_offset = 0;
    mem_mapping_disable(&dev->rom.mem_mapping);

    /* Reset device. */
    ioctl(dev->config.fd, VFIO_DEVICE_RESET);

    /* Enable INTx. */
    vfio_config_writeb(0, 0x05, vfio_config_readb(0, 0x05, dev) | 0x04, dev);
}


static void *
vfio_init(const device_t *info)
{
    vfio_t *dev = (vfio_t *) malloc(sizeof(vfio_t));
    memset(dev, 0, sizeof(vfio_t));

    /* Open VFIO container. */
    int container = open("/dev/vfio/vfio", O_RDWR);
    if (!container) {
	vfio_log("VFIO: Container not found\n");
	return NULL;
    }

    /* Check VFIO API version. */
    int api = ioctl(container, VFIO_GET_API_VERSION);
    if (api != VFIO_API_VERSION) {
	vfio_log("VFIO: Unknown API version %d\n", ioctl(container, VFIO_GET_API_VERSION));
	goto close_container;
    }

    /* Check for Type1 IOMMU support. */
    if (!ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU)) {
	vfio_log("VFIO: Type1 IOMMU not supported\n");
	goto close_container;
    }

    /* Open VFIO group. */
    char fn[32];
    sprintf(fn, "/dev/vfio/%d", info->local);
    int group_fd = open(fn, O_RDWR);
    if (!group_fd) {
	vfio_log("VFIO: Group %d not found\n", info->local);
	goto close_container;
    }

    /* Check if the group is viable. */
    struct vfio_group_status group_status = { .argsz = sizeof(group_status) };
    ioctl(group_fd, VFIO_GROUP_GET_STATUS, &group_status);
    if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
	vfio_log("VFIO: Group %d not viable\n", info->local);
	goto close_group;
    }

    /* Grab the group. */
    ioctl(group_fd, VFIO_GROUP_SET_CONTAINER, &container);
    ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);

    /* Get additional group information. */
    struct vfio_iommu_type1_info iommu_info = { .argsz = sizeof(iommu_info) };
    ioctl(container, VFIO_IOMMU_GET_INFO, &iommu_info);

    /* Map RAM for DMA. */
    vfio_map_dma(container, ram, 0, 1024UL * MIN(mem_size, 1048576));
    if (ram2)
	vfio_map_dma(container, ram2, 1024UL * 1048576, 1024UL * (mem_size - 1048576));

    /* Grab device. */
    int device = ioctl(group_fd, VFIO_GROUP_GET_DEVICE_FD, "0000:07:00.0");
    if (!device) {
	vfio_log("VFIO: Device grab failed\n");
	goto close_group;
    }

    /* Get device information. */
    struct vfio_device_info device_info = { .argsz = sizeof(device_info) };
    ioctl(device, VFIO_DEVICE_GET_INFO, &device_info);

    /* Check if any regions were returned. */
    if (!device_info.num_regions) {
	vfio_log("VFIO: No regions returned, check for allow_unsafe_interrupts message in kernel log\n");
	goto close_group;
    }

    /* Establish region names. */
    int i;
    for (i = 0; i < 6; i++)
	sprintf(dev->bars[i].name, "BAR #%d", dev->bars[i].bar_id = i);
    strcpy(dev->rom.name, "Expansion ROM");
    strcpy(dev->config.name, "Configuration space");
    strcpy(dev->vga_io_lo.name, "VGA 3B0");
    strcpy(dev->vga_io_hi.name, "VGA 3C0");
    strcpy(dev->vga_mem.name, "VGA Framebuffer");

    /* Create IRQ event for later. */
    dev->irq_event = thread_create_event();

    /* Map regions. */
    for (i = 0; i < device_info.num_regions; i++) {
	/* Get region information. */
	struct vfio_region_info reg = { .argsz = sizeof(reg), .index = i };
	ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &reg);

	/* Move on if this is not a valid region. */
	if (!reg.size)
		continue;

	/* Prepare region according to its type. */
	switch (reg.index) {
		case VFIO_PCI_BAR0_REGION_INDEX ... VFIO_PCI_BAR5_REGION_INDEX:
			vfio_prepare_region(dev, device, &reg, &dev->bars[reg.index - VFIO_PCI_BAR0_REGION_INDEX]);
			break;

		case VFIO_PCI_ROM_REGION_INDEX:
			vfio_prepare_region(dev, device, &reg, &dev->rom);
			break;

		case VFIO_PCI_CONFIG_REGION_INDEX:
			vfio_prepare_region(dev, device, &reg, &dev->config);
			break;

		case VFIO_PCI_VGA_REGION_INDEX:
			vfio_prepare_region(dev, device, &reg, &dev->vga_io_lo); /* I/O [3B0:3BB] */
			vfio_prepare_region(dev, device, &reg, &dev->vga_io_hi); /* I/O [3C0:3DF] */
			vfio_prepare_region(dev, device, &reg, &dev->vga_mem); /* memory [A0000:BFFFF] */

			/* This is a video card, inform VGA type and timings. */
			video_inform(VIDEO_FLAG_TYPE_SPECIAL, &timing_default);
			break;

		default:
			vfio_log("VFIO: Unknown region %d (offset %lX) (%d bytes) (%c%c)\n",
				 reg.index, reg.offset, reg.size,
				 (reg.flags & VFIO_REGION_INFO_FLAG_READ) ? 'R' : '-',
				 (reg.flags & VFIO_REGION_INFO_FLAG_WRITE) ? 'W' : '-');
			break;
	}
    }

    /* Make sure we have a valid device. */
    if (!dev->config.fd || !dev->config.read) {
	vfio_log("VFIO: Device has no configuration space region\n");
	goto close_group;
    }

    /* Create eventfd for receiving INTx interrupts. */
    dev->irq_eventfd = eventfd(0, 0);
    if (dev->irq_eventfd == -1)
	fatal("VFIO: eventfd failed (%d)\n", errno);

    /* Mask any existing interrupt. */
    struct vfio_irq_set irq_set = {
	.argsz = sizeof(irq_set),
	.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_MASK,
	.index = VFIO_PCI_INTX_IRQ_INDEX,
	.start = 0,
	.count = 1
    };
    /*if (ioctl(device, VFIO_DEVICE_SET_IRQS, &irq_set))
	fatal("VFIO: IRQ initial mask failed (%d)\n", errno);*/

    /* Add eventfd as an interrupt handler. */
    int32_t *eventfd_ptr;
    irq_set.argsz += sizeof(*eventfd_ptr);
    irq_set.flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
    eventfd_ptr = (int32_t *) &irq_set.data;
    *eventfd_ptr = dev->irq_eventfd;
    if (ioctl(device, VFIO_DEVICE_SET_IRQS, &irq_set))
	fatal("VFIO: IRQ eventfd set failed (%d)\n", errno);

    /* Add PCI card while mapping the configuration space. */
    dev->slot = pci_add_card(PCI_ADD_NORMAL, vfio_config_readb, vfio_config_writeb, dev);

    /* Read IRQ pin. */
    dev->irq_pin = vfio_config_readb(0, 0x3d, dev);
    vfio_log("VFIO: IRQ pin is INT%c\n", '@' + dev->irq_pin);

    /* Start IRQ thread. */
    timer_add(&dev->irq_timer, vfio_irq_timer, dev, 1);
    dev->irq_thread = thread_create(vfio_irq_thread, dev);

    plat_delay_ms(1000);
    irq_set.argsz = sizeof(irq_set);
    irq_set.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER;
    if (ioctl(device, VFIO_DEVICE_SET_IRQS, &irq_set))
	fatal("VFIO: loopback test failed (%d)\n", errno);

    /* Reset device. */
    vfio_log("VFIO: Performing initial reset\n");
    vfio_reset(dev);

    return dev;

close_group:
    close(group_fd);
close_container:
    close(container);
    free(dev);
    return NULL;
}


static void
vfio_close(void *priv)
{
    vfio_t *dev = (vfio_t *) priv;
    if (!dev)
	return;

    /* Reset device. */
    vfio_reset(dev);

    /* Clean up. */
    close(dev->irq_eventfd);
    free(dev);
}


static void
vfio_speed_changed(void *priv)
{
    /* Set operation timings. */
    timing_readb = (int)(pci_timing * timing_default.read_b);
    timing_readw = (int)(pci_timing * timing_default.read_w);
    timing_readl = (int)(pci_timing * timing_default.read_l);
    timing_writeb = (int)(pci_timing * timing_default.write_b);
    timing_writew = (int)(pci_timing * timing_default.write_w);
    timing_writel = (int)(pci_timing * timing_default.write_l);
}


const device_t vfio_device =
{
    "VFIO PCI Passthrough",
    DEVICE_PCI,
    10,
    vfio_init, vfio_close, vfio_reset,
    { NULL },
    vfio_speed_changed,
    NULL,
    NULL
};
