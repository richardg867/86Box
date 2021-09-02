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
#include <86box/config.h>
#include "cpu.h"
#include <86box/device.h>
#include <86box/i2c.h> /* ceilpow2 */
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/pci.h>
#include <86box/plat.h>
#include <86box/timer.h>
#include <86box/video.h>


/* Just so we don't have to include Linux's pci.h, which
   has some defines that conflict with our own pci.h */
#define PCI_SLOT(devfn)	(((devfn) >> 3) & 0x1f)
#define PCI_FUNC(devfn)	((devfn) & 0x07)


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
} vfio_region_t;

typedef struct _vfio_device_ {
    int		fd, slot, irq_pin;
    uint8_t	mem_enabled: 1, io_enabled: 1, rom_enabled: 1,
		can_reset: 1, can_pm_reset: 1, closing: 1,
		pm_cap;
    char	name[13], *rom_fn;

    vfio_region_t bars[6], rom, config, vga_io_lo, vga_io_hi, vga_mem;

    int		irq_eventfd, in_irq, prev_in_irq, irq_active,
		irq_thread_stop;
    thread_t	*irq_thread;
    event_t	*irq_event, *irq_thread_ready, *irq_thread_stopped;
    pc_timer_t	irq_timer;

    struct {
	struct {
		uint8_t state;
		uint32_t offset;
	} nvidia3d0;
    } quirks;

    struct _vfio_device_ *next;
} vfio_device_t;

typedef struct _vfio_group_ {
    int		id, fd;
    uint8_t	hot_reset: 1;

    vfio_device_t *first_device, *current_device;

    struct _vfio_group_ *next;
} vfio_group_t;


static video_timings_t timing_default = {VIDEO_PCI, 8, 16, 32,   8, 16, 32};
static int	container_fd = 0, irq_pending = 0,
		timing_readb = 0, timing_readw = 0, timing_readl = 0,
		timing_writeb = 0, timing_writew = 0, timing_writel = 0;
static vfio_group_t *first_group = NULL, *current_group;


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
    vfio_device_t *dev = (vfio_device_t *) priv;

    /* Read configuration register. */
    register uint32_t ret = vfio_config_read(0, addr, size, dev);
    vfio_log_op("VFIO %s: Config mirror: Read %08X from %02X\n",
		dev->name, ret, addr & 0xff);
    return ret;
}


static void
vfio_quirk_configmirror_write(uint32_t addr, uint32_t val, uint8_t size, void *priv)
{
    vfio_device_t *dev = (vfio_device_t *) priv;

    /* Write configuration register. */
    vfio_log_op("VFIO %s: Config mirror: Read %08X from %02X\n",
		dev->name, val, addr & 0xff);
    vfio_config_write(0, addr, val, size, dev);
}


VFIO_RW_BWL(quirk_configmirror, uint32_t);


static uint32_t
vfio_quirk_nvidia3d0_read(uint16_t addr, uint8_t size, void *priv)
{
    vfio_device_t *dev = (vfio_device_t *) priv;

    /* Cascade to the main handler. */
    uint8_t prev_state = dev->quirks.nvidia3d0.state;
    uint32_t ret = vfio_io_reads_fd(addr, size, &dev->vga_io_hi);
    dev->quirks.nvidia3d0.state = NVIDIA_3D0_NONE;

    /* Interpret NVIDIA commands. */
    if ((addr < 0x3d4) && (prev_state == NVIDIA_3D0_READ) &&
	((dev->quirks.nvidia3d0.offset & 0xffffff00) == 0x00001800)) {
	/* Configuration read. */
	ret = vfio_config_read(0, dev->quirks.nvidia3d0.offset, size, dev);
	vfio_log_op("VFIO %s: NVIDIA 3D0: Read %08X from %08X\n", dev->name,
		    ret, dev->quirks.nvidia3d0.offset & 0xff);
    }

    return ret;
}


static void
vfio_quirk_nvidia3d0_write(uint16_t addr, uint32_t val, uint8_t size, void *priv)
{
    vfio_device_t *dev = (vfio_device_t *) priv;

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
			vfio_log_op("VFIO %s: NVIDIA 3D0: Write %08X to %08X\n", dev->name,
				    val, dev->quirks.nvidia3d0.offset & 0xff);
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
vfio_quirk_configmirror(vfio_device_t *dev, vfio_region_t *bar, uint32_t offset, uint8_t mapping_slot, uint8_t enable)
{
    /* Get the additional memory mapping structure. */
    mem_mapping_t *mapping = &bar->mem_mapping_add[mapping_slot];

    vfio_log("VFIO %s: %sapping configuration space mirror for %s @ %08X\n",
	     dev->name, enable ? "M" : "Unm", bar->name, offset);

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
vfio_quirk_remap(vfio_device_t *dev, vfio_region_t *bar, uint8_t enable)
{
    /* Read vendor ID. */
    uint16_t vendor;
    if (pread(dev->config.fd, &vendor, sizeof(vendor), dev->config.offset) != sizeof(vendor))
	vendor = 0x0000;

    if ((vendor == 0x1002) && (bar->size == 32) &&
	(dev->bars[4].type == 0x01) && (dev->bars[4].size >= 256)) {
	vfio_log("VFIO %s: %sapping ATI 3C3 quirk\n", dev->name, enable ? "M" : "Unm");


    } else if (vendor == 0x10de) {
	/* BAR 0 configuration space mirrors. */
	if (bar->bar_id == 0) {
		vfio_quirk_configmirror(dev, bar, 0x1800, 0, enable);
		vfio_quirk_configmirror(dev, bar, 0x88000, 1, enable);
	}

	/* Port 3D0 configuration space mirror. */
	if ((bar->bar_id == 0xfe) && (bar->size == 32) && dev->bars[1].size) {
		vfio_log("VFIO %s: %sapping NVIDIA 3D0 quirk\n", dev->name, enable ? "M" : "Unm");

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
vfio_bar_gettype(vfio_device_t *dev, vfio_region_t *bar)
{
    /* Read and store BAR type from device if unknown. */
    if (bar->type == 0xff) {
	if (pread(dev->config.fd, &bar->type, sizeof(bar->type),
		  dev->config.offset + 0x10 + (bar->bar_id << 2)) == sizeof(bar->type))
		bar->type &= 0x01;
	else
		bar->type = 0xff;
    }

    /* Return stored BAR type. */
    return bar->type;
}


static void
vfio_bar_remap(vfio_device_t *dev, vfio_region_t *bar, uint32_t new_offset)
{
    vfio_log("VFIO %s: bar_remap(%s, %08X)\n", dev->name, bar->name, new_offset);

    /* Act according to the BAR type. */
    uint8_t bar_type = vfio_bar_gettype(dev, bar);
    uint16_t vga_bitmap;
    if (bar_type == 0x00) { /* Memory BAR */
	if (bar->emulated_offset) {
		vfio_log("VFIO %s: Unmapping %s memory @ %08X-%08X\n", dev->name,
			 bar->name, bar->emulated_offset, bar->emulated_offset + bar->size - 1);

		/* Unmap any quirks. */
		vfio_quirk_remap(dev, bar, 0);

		/* Disable memory mapping. */
		mem_mapping_disable(&bar->mem_mapping);
	}
	/* Expansion ROM requires both ROM enable and memory enable. */
	if (((bar->bar_id != 0xff) || dev->rom_enabled) && dev->mem_enabled && new_offset) {
		vfio_log("VFIO %s: Mapping %s memory @ %08X-%08X\n", dev->name,
			 bar->name, new_offset, new_offset + bar->size - 1);

		/* Enable memory mapping. */
		mem_mapping_set_addr(&bar->mem_mapping, new_offset, bar->size);

		/* Map any quirks. */
		vfio_quirk_remap(dev, bar, 1);
	}
    } else if (bar_type == 0x01) { /* I/O BAR */
	if (bar->emulated_offset) {
		vfio_log("VFIO %s: Unmapping %s I/O @ %04X-%04X\n", dev->name,
			 bar->name, bar->emulated_offset, bar->emulated_offset + bar->size - 1);

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
		vfio_log("VFIO %s: Mapping %s I/O @ %04X-%04X\n", dev->name,
			 bar->name, new_offset, new_offset + bar->size - 1);

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
    vfio_device_t *dev = (vfio_device_t *) priv;
    if (func)
	return 0xff;

    dev->irq_active = 0;

    /* Read register from device. */
    uint8_t ret;
    if (pread(dev->config.fd, &ret, 1, dev->config.offset + addr) != 1) {
	vfio_log("VFIO %s: config_read(%d, %02X) failed\n", dev->name,
		 func, addr);
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
		if (!dev->rom.read) {
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

    vfio_log("VFIO %s: config_read(%02X) = %02X\n", dev->name,
	     addr, ret);

    return ret;
}


static void
vfio_irq_remap(vfio_device_t *dev)
{
    /* Read IRQ pin. */
    dev->irq_pin = vfio_config_readb(0, 0x3d, dev);
    vfio_log("VFIO %s: IRQ pin is INT%c\n", dev->name, '@' + dev->irq_pin);
}


static void
vfio_config_writeb(int func, int addr, uint8_t val, void *priv)
{
    vfio_device_t *dev = (vfio_device_t *) priv;
    if (func)
	return;

    vfio_log("VFIO %s: config_write(%02X, %02X)\n", dev->name, addr, val);

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

		vfio_log("VFIO %s: Command Memory[%d] I/O[%d]\n", dev->name,
			 dev->mem_enabled, dev->io_enabled);

		/* Remap all BARs. */
		for (i = 0; i < 6; i++)
			vfio_bar_remap(dev, &dev->bars[i], dev->bars[i].emulated_offset);

		/* Remap VGA region if that is enabled. */
		if (dev->vga_mem.bar_id) {
			vfio_bar_remap(dev, &dev->vga_io_lo, 0x3b0);
			vfio_bar_remap(dev, &dev->vga_io_hi, 0x3c0);
			vfio_bar_remap(dev, &dev->vga_mem, 0xa0000);
		}
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
		new_offset &= ~(ceilpow2(dev->bars[bar_id].size) - 1);
		vfio_bar_remap(dev, &dev->bars[bar_id], new_offset);
		break;

	case 0x30 ... 0x33: /* Expansion ROM */
		/* Stop if the ROM is absent. */
		if (!dev->rom.read)
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
		new_offset &= ~(ceilpow2(dev->rom.size) - 1);
		vfio_bar_remap(dev, &dev->rom, new_offset);
		break;

	case 0x3d: /* IRQ pin */
		/* Update IRQ pin. */
		vfio_irq_remap(dev);
		break;
    }
}


static void
vfio_irq_thread(void *priv)
{
    vfio_device_t *dev = (vfio_device_t *) priv;
    uint64_t buf;
    struct vfio_irq_set irq_set = {
	.argsz = sizeof(irq_set),
	.flags = 0,
	.index = VFIO_PCI_INTX_IRQ_INDEX,
	.start = 0,
	.count = 1
    };
    int device = dev->config.fd;

    vfio_log("VFIO %s: IRQ thread started\n", dev->name);

    while (!dev->irq_thread_stop) {
	/* Unmask host IRQ. */
	irq_set.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_UNMASK;
	ioctl(device, VFIO_DEVICE_SET_IRQS, &irq_set);

	/* Wait for an interrupt to come in. */
	vfio_log_op("VFIO %s: Waiting for IRQ...\n", dev->name);
	read(dev->irq_eventfd, &buf, sizeof(buf));
	vfio_log_op("VFIO %s: IRQ has arrived: %08lX\n", dev->name, buf);

	/* Mask host IRQ. */
	irq_set.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_MASK;
	ioctl(device, VFIO_DEVICE_SET_IRQS, &irq_set);

	/* Tell the timer to raise the IRQ. */
	dev->in_irq = 1;

	/* Wait for a BAR read/write to lower the IRQ. */
	thread_wait_event(dev->irq_event, -1);
	thread_reset_event(dev->irq_event);
    }

    /* We're done here. */
    thread_set_event(dev->irq_thread_stopped);
    vfio_log("VFIO %s: IRQ thread finished\n", dev->name);
}


static void
vfio_irq_timer(void *priv)
{
    vfio_device_t *dev = (vfio_device_t *) priv;

    /* Schedule next run. */
    timer_on_auto(&dev->irq_timer, 100.0);

    /* Stop if we're not in an IRQ at the moment. */
    if (!dev->in_irq)
	return;

    /* Process an IRQ status change. */
    if (!dev->prev_in_irq) { /* rising edge */
	vfio_log_op("VFIO %s: Raising IRQ on pin INT%c\n", dev->name,
		    '@' + dev->irq_pin);

	/* Raise IRQ. */
	pci_set_irq(dev->slot, dev->irq_pin);

	/* Mark the IRQ as active, so that a BAR read/write can lower it. */
	dev->prev_in_irq = dev->irq_active = 1;
    } else if (!dev->irq_active) { /* falling edge */
	vfio_log_op("VFIO %s: Lowering IRQ on pin INT%c\n", dev->name,
		    '@' + dev->irq_pin);

	/* Lower IRQ. */
	pci_clear_irq(dev->slot, dev->irq_pin);

	/* Mark the IRQ as no longer active. */
	dev->prev_in_irq = dev->irq_active = dev->in_irq = 0;

	/* Unblock the IRQ thread. */
	thread_set_event(dev->irq_event);
    }
}


static void
vfio_irq_enable(vfio_device_t *dev)
{
    vfio_log("VFIO %s: irq_enable()\n", dev->name);

    /* Create eventfd for receiving INTx interrupts. */
    dev->irq_eventfd = eventfd(0, 0);
    if (dev->irq_eventfd == -1) {
	pclog("VFIO %s: eventfd failed (%d)\n", dev->name, errno);
	return;
    }

    /* Mask any existing interrupt. */
    struct vfio_irq_set irq_set = {
	.argsz = sizeof(irq_set),
	.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_MASK,
	.index = VFIO_PCI_INTX_IRQ_INDEX,
	.start = 0,
	.count = 1
    };
    /*if (ioctl(dev->fd, VFIO_DEVICE_SET_IRQS, &irq_set)) {
	pclog("VFIO %s: IRQ initial mask failed (%d)\n", dev->name, errno);
	goto no_irq;
    }*/

    /* Add eventfd as an interrupt handler. */
    int32_t *eventfd_ptr;
    irq_set.argsz += sizeof(*eventfd_ptr);
    irq_set.flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
    eventfd_ptr = (int32_t *) &irq_set.data;
    *eventfd_ptr = dev->irq_eventfd;
    if (ioctl(dev->fd, VFIO_DEVICE_SET_IRQS, &irq_set)) {
	pclog("VFIO %s: IRQ eventfd set failed (%d)\n", dev->name, errno);
	return;
    }

    /* Read IRQ pin. */
    vfio_irq_remap(dev);

    /* Start IRQ thread. */
    dev->irq_thread = thread_create(vfio_irq_thread, dev);

    /* Start IRQ timer. */
    vfio_irq_timer(dev);

#if 0
    irq_set.argsz = sizeof(irq_set);
    irq_set.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER;
    if (ioctl(dev->fd, VFIO_DEVICE_SET_IRQS, &irq_set)) {
	pclog("VFIO %s: IRQ loopback trigger failed (%d)\n", dev->name, errno);
	dev->irq_thread_stop = 1;
	goto no_irq;
    }
#endif
}


static void
vfio_irq_disable(vfio_device_t *dev)
{
    vfio_log("VFIO %s: irq_disable()\n", dev->name);

    /* Stop IRQ timer. */
    timer_on_auto(&dev->irq_timer, 0.0);

    /* Stop IRQ thread. */
    if (dev->irq_thread) {
	dev->irq_thread_stop = 1;
	uint64_t count = 1;
	write(dev->irq_eventfd, &count, sizeof(count));
	thread_set_event(dev->irq_event);
	thread_wait_event(dev->irq_thread_stopped, -1);
	dev->irq_thread = NULL;
    }

    /* Clear any pending IRQs. */
    dev->in_irq = dev->prev_in_irq = dev->irq_active = 0;

    /* Close eventfd. */
    if (dev->irq_eventfd >= 0) {
	close(dev->irq_eventfd);
	dev->irq_eventfd = -1;
    }
}


static void
vfio_prepare_region(vfio_device_t *dev, struct vfio_region_info *reg, vfio_region_t *region)
{
    /* Set region structure information. */
    region->fd = dev->fd;
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

    /* Use special memory mapping for expansion ROMs. */
    if (reg->index == VFIO_PCI_ROM_REGION_INDEX) {
	/* Use MMIO only. */
	region->fd = -1;

	/* Open ROM file if one was given. */
	FILE *f = NULL;
	if (dev->rom_fn) {
		f = fopen(dev->rom_fn, "rb");
		if (f) {
			/* Determine region size if the device has no ROM region. */
			if (!region->size) {
				fseek(f, 0, SEEK_END);
				region->size = ceilpow2(ftell(f));
				if (region->size < 2048)
					region->size = 2048;
				fseek(f, 0, SEEK_SET);
			}
		} else {
			/* Fall back to the device's ROM if it has one. */
			pclog("VFIO %s: Could not read ROM file: %s\n", dev->name, dev->rom_fn);
			if (region->size) {
				pclog("VFIO %s: Falling back to device ROM\n", dev->name);
			} else {
				/* Disable ROM. */
				pclog("VFIO %s: Not enabling ROM\n", dev->name);
				region->read = region->write = 0;
				goto end;
			}
		}
	}

	/* Mark this as the expansion ROM region. */
	region->type = 0x00;
	region->bar_id = 0xff;

	/* Allocate ROM shadow area. */
	region->mmap_base = region->mmap_precalc = (uint8_t *) malloc(region->size);
	if (!region->mmap_base) {
		vfio_log("\n");
		pclog("VFIO %s: ROM malloc(%d) failed\n", dev->name, region->size);
		goto end;
	}
	memset(region->mmap_base, 0xff, region->size);

	int i, j = 0;
	if (f) {
		/* Read ROM from file. */
		while ((i = fread(region->mmap_precalc, 1,
				  region->size - j,
				  f)) != 0) {
			region->mmap_precalc += i;
			j += i;
		}
		fclose(f);
	} else {
		/* Read ROM from device. */
		while ((i = pread(dev->fd, region->mmap_precalc,
				  region->size - j,
				  region->offset + j)) != 0) {
			region->mmap_precalc += i;
			j += i;
		}
	}
    } else {
	/* Attempt to mmap the region. */
	region->mmap_base = mmap(NULL, region->size,
				 (region->read ? PROT_READ : 0) |
				 (region->write ? PROT_WRITE : 0),
				 MAP_SHARED, region->fd, region->offset);
	if (region->mmap_base == ((void *) -1)) /* mmap failed */
		region->mmap_base = NULL;
    }
    region->mmap_precalc = region->mmap_base;

end:
    vfio_log("VFIO %s: Region: %s (offset %lX) (%d bytes) ", dev->name,
	     region->name, region->offset, region->size);

    /* Create memory mapping for if we need it. */
    if (region->mmap_base) { /* mmap available */
	vfio_log("(MM)");
	mem_mapping_add(&region->mem_mapping, 0, 0,
			region->read ? vfio_mem_readb_mm : NULL,
			region->read ? vfio_mem_readw_mm : NULL,
			region->read ? vfio_mem_readl_mm : NULL,
			region->write ? vfio_mem_writeb_mm : NULL,
			region->write ? vfio_mem_writew_mm : NULL,
			region->write ? vfio_mem_writel_mm : NULL,
			NULL, MEM_MAPPING_EXTERNAL, region);
    } else if (region->fd >= 0) { /* mmap not available, but fd is */
	vfio_log("(FD)");
	mem_mapping_add(&region->mem_mapping, 0, 0,
			region->read ? vfio_mem_readb_fd : NULL,
			region->read ? vfio_mem_readw_fd : NULL,
			region->read ? vfio_mem_readl_fd : NULL,
			region->write ? vfio_mem_writeb_fd : NULL,
			region->write ? vfio_mem_writew_fd : NULL,
			region->write ? vfio_mem_writel_fd : NULL,
			NULL, MEM_MAPPING_EXTERNAL, region);
    } else {
	vfio_log("(not mapped)");
    }

    vfio_log(" (%c%c)\n", region->read ? 'R' : '-', region->write ? 'W' : '-');
}


static vfio_group_t *
vfio_get_group(int id, uint8_t add)
{
    /* Look for an existing group. */
    vfio_group_t *group = first_group;
    while (group) {
	if (group->id == id)
		return group;
	else if (group->next)
		group = group->next;
	else
		break;
    }

    /* Don't add a group if told not to. */
    if (!add)
	return NULL;

    /* Add group if no matches were found. */
    if (group) {
	group->next = (vfio_group_t *) malloc(sizeof(vfio_group_t));
	group = group->next;
    } else {
	group = first_group = (vfio_group_t *) malloc(sizeof(vfio_group_t));
    }
    memset(group, 0, sizeof(vfio_group_t));
    group->id = id;

    /* Open VFIO group. */
    char group_file[32];
    snprintf(group_file, sizeof(group_file), "/dev/vfio/%d", group->id);
    group->fd = open(group_file, O_RDWR);
    if (group->fd < 0) {
	pclog("VFIO: Group %d not found\n", group->id);
	goto end;
    }

    /* Check if the group is viable. */
    struct vfio_group_status group_status = { .argsz = sizeof(group_status) };
    if (ioctl(group->fd, VFIO_GROUP_GET_STATUS, &group_status)) {
	pclog("VFIO: Group %d GET_STATUS failed (%d)\n", group->id, errno);
	goto close_fd;
    } else if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
	pclog("VFIO: Group %d not viable\n", group->id);
	goto close_fd;
    }

    /* Claim the group. */
    if (ioctl(group->fd, VFIO_GROUP_SET_CONTAINER, &container_fd)) {
	pclog("VFIO: Group %d SET_CONTAINER failed\n", group->id);
	goto close_fd;
    }

    goto end;

close_fd:
    close(group->fd);
    group->fd = -1;
end:
    return group;
}


static void
vfio_dev_prereset(vfio_device_t *dev)
{
    vfio_log("VFIO %s: prereset()\n", dev->name);

    /* Disable interrupts. */
    vfio_irq_disable(dev);

    /* Extra steps for devices with power management capability. */
    if (dev->pm_cap) {
	/* Make sure the device is in D0 state. */
	uint8_t pm_ctrl = vfio_config_readb(0, dev->pm_cap + 0x04, dev),
		state = pm_ctrl & 0x03;
	if (state) {
		pm_ctrl &= ~0x03;
		vfio_config_writeb(0, dev->pm_cap + 0x04, pm_ctrl, dev);

		pm_ctrl = vfio_config_readb(0, dev->pm_cap + 0x04, dev);
		state = pm_ctrl & 0x03;
		if (state)
			vfio_log("VFIO %s: Device stuck in D%d state\n", dev->name, state);
	}

	/* Enable PM reset if the device supports it. */
	dev->can_pm_reset = !(pm_ctrl & 0x08);
    }

    /* Disable bus master, BARs, expansion ROM and VGA. */
    vfio_config_writeb(0, 0x04, vfio_config_readb(0, 0x04, dev) & ~0x07, dev);

    /* Enable INTx. */
    vfio_config_writeb(0, 0x05, vfio_config_readb(0, 0x05, dev) & ~0x04, dev);
}


static void
vfio_dev_postreset(vfio_device_t *dev)
{
    vfio_log("VFIO %s: postreset()\n", dev->name);

    /* Enable interrupts. */
    if (!dev->closing)
	vfio_irq_enable(dev);

    /* Reset BARs, whatever this does. */
    uint32_t val = 0;
    for (int i = 0x10; i < 0x28; i++)
	pwrite(dev->config.fd, &val, sizeof(val), dev->config.offset + i);
}


static void
vfio_dev_reset(void *priv)
{
    vfio_device_t *dev = (vfio_device_t *) priv;
    if (!dev)
	return;
    vfio_log("VFIO %s: reset()\n", dev->name);

    /* Pre-reset ourselves. */
    vfio_dev_prereset(dev);

    /* Get hot reset information. */
    struct vfio_pci_hot_reset_info *hot_reset_info;
    hot_reset_info = (struct vfio_pci_hot_reset_info *) malloc(sizeof(struct vfio_pci_hot_reset_info));
    memset(hot_reset_info, 0, sizeof(struct vfio_pci_hot_reset_info));
    hot_reset_info->argsz = sizeof(struct vfio_pci_hot_reset_info);
    if (ioctl(dev->fd, VFIO_DEVICE_GET_PCI_HOT_RESET_INFO, hot_reset_info) &&
	(errno != ENOSPC)) {
	vfio_log("VFIO %s: GET_PCI_HOT_RESET_INFO 1 failed (%d)\n", dev->name, errno);
	goto free_hot_reset_info;
    }

    /* Get hot reset information a second time, now
       with enough room for the dependent device list. */
    int count = hot_reset_info->count,
	size = sizeof(struct vfio_pci_hot_reset_info) + (sizeof(struct vfio_pci_dependent_device) * count);
    free(hot_reset_info);
    hot_reset_info = (struct vfio_pci_hot_reset_info *) malloc(size);
    memset(hot_reset_info, 0, size);
    hot_reset_info->argsz = size;
    if (ioctl(dev->fd, VFIO_DEVICE_GET_PCI_HOT_RESET_INFO, hot_reset_info)) {
	vfio_log("VFIO %s: GET_PCI_HOT_RESET_INFO 2 failed (%d)\n", dev->name, errno);
	goto free_hot_reset_info;
    }
    struct vfio_pci_dependent_device *devices = &hot_reset_info->devices[0];

    /* Pre-reset affected devices. */
    char name[13];
    int i;
    vfio_group_t *group;
    vfio_device_t *dep_dev;
    for (i = 0; i < hot_reset_info->count; i++) {
	/* Build this dependent device's name. */
	snprintf(name, sizeof(name), "%04x:%02x:%02x.%1x",
		 devices[i].segment, devices[i].bus, PCI_SLOT(devices[i].devfn), PCI_FUNC(devices[i].devfn));

	/* Check if we own this device's group. */
	if (!(group = vfio_get_group(devices[i].group_id, 0))) {
		vfio_log("VFIO %s: Cannot hot reset; we don't own group %d for dependent device %s\n",
			 dev->name, devices[i].group_id, name);

		/* Remove hot reset flag from all groups. */
		group = first_group;
		while (group) {
			group->hot_reset = 0;
			group = group->next;
		}

		goto free_hot_reset_info;
	}

	/* Mark that this group should be hot reset. */
	group->hot_reset = 1;

	/* Don't pre-reset ourselves again. */
	if (!strcasecmp(name, dev->name))
		continue;
	vfio_log("VFIO %s: Resetting dependent device %s\n", dev->name, name);

	/* Find this device's structure within the group and pre-reset it. */
	dep_dev = group->first_device;
	while (dep_dev) {
		if (!strcasecmp(name, dep_dev->name)) {
			vfio_dev_prereset(dep_dev);
			break;
		}
		dep_dev = dep_dev->next;
	}
    }

    /* Count the amount of group fds to reset. */
    count = 0;
    group = first_group;
    while (group) {
	if (group->hot_reset)
		count++;
	group = group->next;
    }

    /* Allocate hot reset structure. */
    struct vfio_pci_hot_reset *hot_reset;
    size = sizeof(struct vfio_pci_hot_reset) + (sizeof(int32_t) * count);
    hot_reset = (struct vfio_pci_hot_reset *) malloc(size);
    memset(hot_reset, 0, size);
    hot_reset->argsz = size;
    int32_t *fds = &hot_reset->group_fds[0];

    /* Add group fds. */
    group = first_group;
    while (group) {
	if (group->hot_reset) {
		fds[hot_reset->count++] = group->fd;
		group->hot_reset = 0;
	}
	group = group->next;
    }

    /* Trigger reset. */
    if (ioctl(dev->fd, VFIO_DEVICE_PCI_HOT_RESET, hot_reset)) {
	vfio_log("VFIO %s: PCI_HOT_RESET failed (%d)\n", dev->name, errno);
    } else {
	vfio_log("VFIO %s: Hot reset successful\n", dev->name);

	/* Don't PM reset this device. */
	dev->can_pm_reset = 0;
    }
    free(hot_reset);

    /* Post-reset affected devices. */
    for (i = 0; i < hot_reset_info->count; i++) {
	/* Build this dependent device's name. */
	snprintf(name, sizeof(name), "%04x:%02x:%02x.%1x",
		 devices[i].segment, devices[i].bus, PCI_SLOT(devices[i].devfn), PCI_FUNC(devices[i].devfn));

	/* Don't post-reset ourselves yet. */
	if (!strcasecmp(name, dev->name))
		continue;

	/* Get this device's group. */
	if (!(group = vfio_get_group(devices[i].group_id, 0)))
		continue;

	/* Find this device within the group and post-reset it. */
	dep_dev = group->first_device;
	while (dep_dev) {
		if (!strcasecmp(name, dep_dev->name)) {
			vfio_dev_postreset(dep_dev);
			break;
		}
		dep_dev = dep_dev->next;
	}
    }

free_hot_reset_info:
    free(hot_reset_info);

    /* PM reset device if supported and required. */
    if (dev->can_pm_reset) {
	if (ioctl(dev->fd, VFIO_DEVICE_RESET))
		vfio_log("VFIO %s: DEVICE_RESET failed (%d)\n", dev->name, errno);
	else
		vfio_log("VFIO %s: PM reset successful\n", dev->name);
    }

    /* Post-reset ourselves. */
    vfio_dev_postreset(dev);
}


static void *
vfio_dev_init(const device_t *info)
{
    vfio_device_t *dev = current_group->current_device;
    vfio_log("VFIO %s: init()\n", dev->name);

    /* Grab device. */
    dev->fd = ioctl(current_group->fd, VFIO_GROUP_GET_DEVICE_FD, dev->name);
    if (dev->fd < 0) {
	vfio_log("VFIO %s: GET_DEVICE_FD failed (%d)\n", dev->name, errno);
	goto end;
    }

    /* Get device information. */
    struct vfio_device_info device_info = { .argsz = sizeof(device_info) };
    if (ioctl(dev->fd, VFIO_DEVICE_GET_INFO, &device_info)) {
	pclog("VFIO %s: GET_INFO failed (%d), check for error in kernel log\n", dev->name, errno);
	goto end;
    }

    /* Check if any regions were returned. */
    if (!device_info.num_regions) {
	pclog("VFIO %s: No regions returned, check for error in kernel log\n", dev->name);
	goto end;
    }

    /* Set reset flag. */
    dev->can_reset = !!(device_info.flags & VFIO_DEVICE_FLAGS_RESET);

    /* Establish region names. */
    int i;
    for (i = 0; i < 6; i++)
	sprintf(dev->bars[i].name, "BAR #%d", dev->bars[i].bar_id = i);
    strcpy(dev->rom.name, "Expansion ROM");
    strcpy(dev->config.name, "Configuration space");
    strcpy(dev->vga_io_lo.name, "VGA 3B0");
    strcpy(dev->vga_io_hi.name, "VGA 3C0");
    strcpy(dev->vga_mem.name, "VGA Framebuffer");

    /* Prepare all regions. */
    struct vfio_region_info reg = { .argsz = sizeof(reg) };
    for (i = 0; i < device_info.num_regions; i++) {
	/* Get region information. */
	reg.index = i;
	ioctl(dev->fd, VFIO_DEVICE_GET_REGION_INFO, &reg);

	/* Move on to the next region if this one is not valid. */
	if (!reg.size)
		continue;

	/* Prepare region according to its type. */
	switch (reg.index) {
		case VFIO_PCI_BAR0_REGION_INDEX ... VFIO_PCI_BAR5_REGION_INDEX:
			vfio_prepare_region(dev, &reg, &dev->bars[reg.index - VFIO_PCI_BAR0_REGION_INDEX]);
			break;

		case VFIO_PCI_ROM_REGION_INDEX:
			vfio_prepare_region(dev, &reg, &dev->rom);
			break;

		case VFIO_PCI_CONFIG_REGION_INDEX:
			vfio_prepare_region(dev, &reg, &dev->config);
			break;

		case VFIO_PCI_VGA_REGION_INDEX:
			/* Don't claim VGA region if an emulated video card is present. */
			if (gfxcard != VID_NONE) {
				vfio_log("VFIO %s: Skipping VGA region due to emulated video card\n",
					 dev->name);
				break;
			}

			vfio_prepare_region(dev, &reg, &dev->vga_io_lo); /* I/O [3B0:3BB] */
			vfio_prepare_region(dev, &reg, &dev->vga_io_hi); /* I/O [3C0:3DF] */
			vfio_prepare_region(dev, &reg, &dev->vga_mem); /* memory [A0000:BFFFF] */

			/* Inform that a PCI VGA video card is attached. */
			video_inform(VIDEO_FLAG_TYPE_SPECIAL, &timing_default);
			break;

		default:
			vfio_log("VFIO %s: Unknown region %d (offset %lX) (%d bytes) (%c%c)\n",
				 dev->name, reg.index, reg.offset, reg.size,
				 (reg.flags & VFIO_REGION_INFO_FLAG_READ) ? 'R' : '-',
				 (reg.flags & VFIO_REGION_INFO_FLAG_WRITE) ? 'W' : '-');
			break;
	}
    }

    /* Make sure we have a valid device. */
    if (!dev->config.fd || !dev->config.read) {
	pclog("VFIO %s: No configuration space region\n", dev->name);
	goto end;
    }

    /* Identify PCI capabilities we care about, storing the offsets for them. */
    if (vfio_config_readb(0, 0x06, dev) & 0x10) {
	/* Read pointer to the first capability. */
	uint8_t cap_ptr = vfio_config_readb(0, 0x34, dev), cap_id;
	while (cap_ptr && (cap_ptr != 0xff)) {
		cap_id = vfio_config_readb(0, cap_ptr, dev);
		if (cap_id == 0x01)
			dev->pm_cap = cap_ptr;

		/* Move on to the next capability. */
		cap_ptr = vfio_config_readb(0, cap_ptr + 1, dev);
	}
    }

    /* Prepare a dummy region if the device has no
       ROM region and we're loading a ROM from file. */
    if (dev->rom_fn && !dev->rom.read) {
	reg.index = VFIO_PCI_ROM_REGION_INDEX;
	reg.offset = reg.size = 0;
	reg.flags = VFIO_REGION_INFO_FLAG_READ;
	vfio_prepare_region(dev, &reg, &dev->rom);
    }

    /* Add PCI card while mapping the configuration space. */
    dev->slot = pci_add_card(PCI_ADD_NORMAL, vfio_config_readb, vfio_config_writeb, dev);

    /* Initialize IRQ stuff. */
    dev->irq_event = thread_create_event();
    dev->irq_thread_stopped = thread_create_event();
    timer_add(&dev->irq_timer, vfio_irq_timer, dev, 0);

    /* Reset device. This should also enable IRQs. */
    vfio_log("VFIO %s: Performing initial reset\n", dev->name);
    vfio_dev_reset(dev);

    return dev;

end:
    if (dev->fd >= 0)
	close(dev->fd);
    return NULL;
}


static void
vfio_dev_close(void *priv)
{
    vfio_device_t *dev = (vfio_device_t *) priv;
    if (!dev)
	return;
    vfio_log("VFIO %s: close()\n", dev->name);

    /* Reset device. */
    dev->closing = 1;
    vfio_dev_reset(dev);

    /* Clean up. */
    if (dev->fd >= 0) {
	close(dev->fd);
	dev->fd = -1;
    }
}


static void
vfio_dev_speed_changed(void *priv)
{
    /* Set operation timings. */
    timing_readb = (int)(pci_timing * timing_default.read_b);
    timing_readw = (int)(pci_timing * timing_default.read_w);
    timing_readl = (int)(pci_timing * timing_default.read_l);
    timing_writeb = (int)(pci_timing * timing_default.write_b);
    timing_writew = (int)(pci_timing * timing_default.write_w);
    timing_writel = (int)(pci_timing * timing_default.write_l);
}


static const device_t vfio_device =
{
    "VFIO PCI Passthrough",
    DEVICE_PCI,
    0,
    vfio_dev_init, vfio_dev_close, vfio_dev_reset,
    { NULL },
    vfio_dev_speed_changed,
    NULL,
    NULL
};


void
vfio_unmap_dma(uint32_t offset, uint32_t size)
{
    struct vfio_iommu_type1_dma_unmap dma_unmap = {
	.argsz = sizeof(dma_unmap),
	.iova = offset,
	.size = size
    };

    vfio_log("VFIO: unmap_dma(%08X, %d)\n", offset, size);

    /* Unmap DMA region. */
    if (!ioctl(container_fd, VFIO_IOMMU_UNMAP_DMA, &dma_unmap))
	return;

    vfio_log("VFIO: unmap_dma(%08X, %d) failed (%d)\n", offset, size, errno);
}


void
vfio_map_dma(uint8_t *ptr, uint32_t offset, uint32_t size)
{
    struct vfio_iommu_type1_dma_map dma_map = {
	.argsz = sizeof(dma_map),
	.vaddr = (uint64_t) ptr,
	.iova = offset,
	.size = size,
	.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE
    };

    vfio_log("VFIO: map_dma(%lX, %08X, %d)\n", ptr, offset, size);

    /* Map DMA region. */
    if (!ioctl(container_fd, VFIO_IOMMU_MAP_DMA, &dma_map))
	return;

    /* QEMU says the mapping should be retried in case of EBUSY. */
    if (errno == EBUSY) {
	vfio_unmap_dma(offset, size);
	if (!ioctl(container_fd, VFIO_IOMMU_MAP_DMA, &dma_map))
		return;
    }

    /*fatal*/pclog("VFIO: map_dma(%lX, %08X, %d) failed (%d)\n", ptr, offset, size, errno);
}


void
vfio_init()
{
    vfio_log("VFIO: init()\n");

    /* Stay quiet if VFIO is not configured. */
    char *category = "VFIO",
	 *devices = config_get_string(category, "devices", NULL);
    if (!devices || !strlen(devices))
	return;

    /* Open VFIO container. */
    container_fd = open("/dev/vfio/vfio", O_RDWR);
    if (container_fd < 0) {
	pclog("VFIO: Container not found (is vfio-pci loaded?)\n");
	return;
    }

    /* Check VFIO API version. */
    int api = ioctl(container_fd, VFIO_GET_API_VERSION);
    if (api != VFIO_API_VERSION) {
	pclog("VFIO: Unknown API version %d (expected %d)\n", api, VFIO_API_VERSION);
	goto close_container;
    }

    /* Check for Type1 IOMMU support. */
    if (!ioctl(container_fd, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU)) {
	pclog("VFIO: Type1 IOMMU not supported\n");
	goto close_container;
    }

    /* Parse device list. */
    char *strtok_save, *token = strtok_r(devices, " ", &strtok_save),
	 dev_name[13], sysfs_device[46], sysfs_group[256], *group_name,
	 config_key[32];
    int i;
    vfio_device_t *dev = NULL, *prev_dev;

    while (token) {
	/* Prepend 0000: to device name if required. */
	snprintf(dev_name, sizeof(dev_name),
		 (strchr(token, ':') == strrchr(token, ':')) ? "0000:%s" : "%s",
		 token);
	pclog("VFIO %s: ", dev_name);

	/* Read iommu_group sysfs symlink for this device. */
	snprintf(sysfs_device, sizeof(sysfs_device),
		 "/sys/bus/pci/devices/%s/iommu_group",
		 dev_name);
	if ((i = readlink(sysfs_device, sysfs_group, sizeof(sysfs_group) - 1)) > 0) {
		/* Determine group ID. */
		sysfs_group[i] = '\0';
		group_name = sysfs_group;
		do {
			group_name = strrchr(sysfs_group, '/');
			if (group_name) {
				group_name[0] = '\0'; 
				group_name++;
			} else {
				group_name = sysfs_group;
				break;
			}
		} while (group_name[0] == '\0');

		/* Parse group ID. */
		if (sscanf(group_name, "%d", &i) != 1) {
			pclog("Could not parse IOMMU group ID: %s\n", group_name);
			goto next;
		}

		pclog("IOMMU group %d\n", i);
	} else {
		/* No symlink found, move on to the next device. */
		pclog("Device not found\n");
		goto next;
	}

	/* Get group by ID, and move on to the next device
	   if the group failed to initialize. (Not viable, etc.) */
	current_group = vfio_get_group(i, 1);
	if (current_group->fd < 0) {
		pclog("VFIO %s: Skipping because group failed to initialize\n", dev_name);
		goto next;
	}

	/* Allocate device structure. */
	prev_dev = current_group->current_device;
	dev = current_group->current_device = (vfio_device_t *) malloc(sizeof(vfio_device_t));
	memset(dev, 0, sizeof(vfio_device_t));
	strncpy(dev->name, dev_name, sizeof(dev->name) - 1);

	/* Read device-specific settings. */
	sprintf(config_key, "%s_rom_fn", token);
	dev->rom_fn = config_get_string(category, config_key, NULL);
	if (dev->rom_fn)
		pclog("VFIO %s: Loading ROM from file: %s\n", dev_name, dev->rom_fn);

	/* Add to linked device list. */
	if (prev_dev)
		prev_dev->next = dev;
	else
		current_group->first_device = dev;
next:
	token = strtok_r(NULL, " ", &strtok_save);
    }

    /* Stop if no devices were added. */
    if (!dev)
	goto close_container;

    /* Set IOMMU type. */
    if (ioctl(container_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU)) {
	pclog("VFIO: SET_IOMMU failed (%d)\n", errno);
	goto close_container;
    }

    /* Map RAM to container for DMA. */
    vfio_map_dma(ram, 0, 1024UL * MIN(mem_size, 1048576));
    if (ram2)
	vfio_map_dma(ram2, 1024UL * 1048576, 1024UL * (mem_size - 1048576));

    /* Initialize all devices. */
    int inst = 0;
    current_group = first_group;
    while (current_group) {
	dev = current_group->first_device;
	while (dev) {
		if (device_add_inst(&vfio_device, inst++) == dev) {
			/* Add to linked device list. */
			if (prev_dev)
				prev_dev->next = dev;
			else
				current_group->first_device = dev;
		} else {
			current_group->current_device = prev_dev;
			inst--;
			pclog("VFIO %s: device_add_inst(%d) failed\n", dev_name, inst);
			free(dev);
		}
		dev = dev->next;
	}
	current_group = current_group->next;
    }

    return;

close_container:
    close(container_fd);
    container_fd = -1;
}


void
vfio_close()
{
    vfio_log("VFIO: close()\n");

    /* Free all groups. */
    while (first_group) {
	current_group = first_group;

	/* Free all devices. */
	while (current_group->first_device) {
		current_group->current_device = current_group->first_device;

		if (current_group->current_device->fd >= 0)
			close(current_group->current_device->fd);

		current_group->first_device = current_group->current_device->next;
		free(current_group->current_device);
	}

	if (current_group->fd >= 0)
		close(current_group->fd);

	first_group = current_group->next;
	free(current_group);
    }

    /* Close container. */
    if (container_fd >= 0) {
	close(container_fd);
	container_fd = 0;
    }
}
