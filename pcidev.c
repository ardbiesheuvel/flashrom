/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2009 Uwe Hermann <uwe@hermann-uwe.de>
 * Copyright (C) 2010, 2011 Carl-Daniel Hailfinger
 * Copyright (C) 2018 Luc Verhaegen <libv@skynet.be>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "flash.h"
#include "programmer.h"
#include "hwaccess.h"

struct pci_access *pacc;

enum pci_bartype {
	TYPE_MEMBAR,
	TYPE_IOBAR,
	TYPE_ROMBAR,
	TYPE_UNKNOWN
};

uintptr_t pcidev_readbar(struct pci_dev *dev, int bar)
{
	uint64_t addr;
	uint32_t upperaddr;
	uint8_t headertype;
	uint16_t supported_cycles;
	enum pci_bartype bartype = TYPE_UNKNOWN;


	headertype = pci_read_byte(dev, PCI_HEADER_TYPE) & 0x7f;
	msg_pspew("PCI header type 0x%02x\n", headertype);

	/* Don't use dev->base_addr[x] (as value for 'bar'), won't work on older libpci. */
	addr = pci_read_long(dev, bar);

	/* Sanity checks. */
	switch (headertype) {
	case PCI_HEADER_TYPE_NORMAL:
		switch (bar) {
		case PCI_BASE_ADDRESS_0:
		case PCI_BASE_ADDRESS_1:
		case PCI_BASE_ADDRESS_2:
		case PCI_BASE_ADDRESS_3:
		case PCI_BASE_ADDRESS_4:
		case PCI_BASE_ADDRESS_5:
			if ((addr & PCI_BASE_ADDRESS_SPACE) == PCI_BASE_ADDRESS_SPACE_IO)
				bartype = TYPE_IOBAR;
			else
				bartype = TYPE_MEMBAR;
			break;
		case PCI_ROM_ADDRESS:
			bartype = TYPE_ROMBAR;
			break;
		}
		break;
	case PCI_HEADER_TYPE_BRIDGE:
		switch (bar) {
		case PCI_BASE_ADDRESS_0:
		case PCI_BASE_ADDRESS_1:
			if ((addr & PCI_BASE_ADDRESS_SPACE) == PCI_BASE_ADDRESS_SPACE_IO)
				bartype = TYPE_IOBAR;
			else
				bartype = TYPE_MEMBAR;
			break;
		case PCI_ROM_ADDRESS1:
			bartype = TYPE_ROMBAR;
			break;
		}
		break;
	case PCI_HEADER_TYPE_CARDBUS:
		break;
	default:
		msg_perr("Unknown PCI header type 0x%02x, BAR type cannot be determined reliably.\n",
			 headertype);
		break;
	}

	supported_cycles = pci_read_word(dev, PCI_COMMAND);

	msg_pdbg("Requested BAR is of type ");
	switch (bartype) {
	case TYPE_MEMBAR:
		msg_pdbg("MEM");
		if (!(supported_cycles & PCI_COMMAND_MEMORY)) {
			msg_perr("MEM BAR access requested, but device has MEM space accesses disabled.\n");
			/* TODO: Abort here? */
		}
		msg_pdbg(", %sbit, %sprefetchable\n",
			 ((addr & 0x6) == 0x0) ? "32" : (((addr & 0x6) == 0x4) ? "64" : "reserved"),
			 (addr & 0x8) ? "" : "not ");
		if ((addr & 0x6) == 0x4) {
			/* The spec says that a 64-bit register consumes
			 * two subsequent dword locations.
			 */
			upperaddr = pci_read_long(dev, bar + 4);
			if (upperaddr != 0x00000000) {
				/* Fun! A real 64-bit resource. */
				if (sizeof(uintptr_t) != sizeof(uint64_t)) {
					msg_perr("BAR unreachable!");
					/* TODO: Really abort here? If multiple PCI devices match,
					 * we might never tell the user about the other devices.
					 */
					return 0;
				}
				addr |= (uint64_t)upperaddr << 32;
			}
		}
		addr &= PCI_BASE_ADDRESS_MEM_MASK;
		break;
	case TYPE_IOBAR:
		msg_pdbg("I/O\n");
#if __FLASHROM_HAVE_OUTB__
		if (!(supported_cycles & PCI_COMMAND_IO)) {
			msg_perr("I/O BAR access requested, but device has I/O space accesses disabled.\n");
			/* TODO: Abort here? */
		}
#else
		msg_perr("I/O BAR access requested, but flashrom does not support I/O BAR access on this "
			 "platform (yet).\n");
#endif
		addr &= PCI_BASE_ADDRESS_IO_MASK;
		break;
	case TYPE_ROMBAR:
		msg_pdbg("ROM\n");
		/* Not sure if this check is needed. */
		if (!(supported_cycles & PCI_COMMAND_MEMORY)) {
			msg_perr("MEM BAR access requested, but device has MEM space accesses disabled.\n");
			/* TODO: Abort here? */
		}
		addr &= PCI_ROM_ADDRESS_MASK;
		break;
	case TYPE_UNKNOWN:
		msg_perr("BAR type unknown, please report a bug at flashrom@flashrom.org\n");
	}

	return (uintptr_t)addr;
}

static int pcidev_shutdown(void *data)
{
	if (pacc == NULL) {
		msg_perr("%s: Tried to cleanup an invalid PCI context!\n"
			 "Please report a bug at flashrom@flashrom.org\n", __func__);
		return 1;
	}
	pci_cleanup(pacc);
	pacc = NULL;
	return 0;
}

int pci_init_common(void)
{
	struct pci_dev *dev;

	if (pacc != NULL) {
		msg_perr("%s: Tried to allocate a new PCI context, but there is still an old one!\n"
			 "Please report a bug at flashrom@flashrom.org\n", __func__);
		return 1;
	}
	pacc = pci_alloc();     /* Get the pci_access structure */
	pci_init(pacc);         /* Initialize the PCI library */
	if (register_shutdown(pcidev_shutdown, NULL))
		return 1;
	pci_scan_bus(pacc);     /* We want to get the list of devices */
	for (dev=pacc->devices; dev; dev=dev->next)     /* Iterate over all devices */
	{
		pci_fill_info(dev, PCI_FILL_IDENT | PCI_FILL_BASES | PCI_FILL_CLASS);   /* Fill in header info we need */
	}
	return 0;
}

/* pcidev_init gets an array of allowed PCI device IDs and returns a pointer to struct pci_dev iff exactly one
 * match was found. If the "pci=bb:dd.f" programmer parameter was specified, a match is only considered if it
 * also matches the specified bus:device.function.
 * For convenience, this function also registers its own undo handlers.
 */
struct pci_dev *pcidev_init(const struct dev_entry *devs, int bar)
{
	struct pci_dev *dev;
	struct pci_dev *found_dev = NULL;
	struct pci_filter filter;
	char *pcidev_bdf;
	char *msg = NULL;
	int found = 0;
	int i;
	uintptr_t addr = 0;

	if (pci_init_common() != 0)
		return NULL;
	pci_filter_init(pacc, &filter);

	/* Filter by bb:dd.f (if supplied by the user). */
	pcidev_bdf = extract_programmer_param("pci");
	if (pcidev_bdf != NULL) {
		if ((msg = pci_filter_parse_slot(&filter, pcidev_bdf))) {
			msg_perr("Error: %s\n", msg);
			return NULL;
		}
	}
	free(pcidev_bdf);

	for (dev = pacc->devices; dev; dev = dev->next) {
		if (pci_filter_match(&filter, dev)) {
			/* Check against list of supported devices. */
			for (i = 0; devs[i].device_name != NULL; i++)
				if ((dev->vendor_id == devs[i].vendor_id) &&
				    (dev->device_id == devs[i].device_id))
					break;
			/* Not supported, try the next one. */
			if (devs[i].device_name == NULL)
				continue;

			msg_pdbg("Found \"%s %s\" (%04x:%04x, BDF %02x:%02x.%x).\n", devs[i].vendor_name,
				 devs[i].device_name, dev->vendor_id, dev->device_id, dev->bus, dev->dev,
				 dev->func);
			if (devs[i].status == NT)
				msg_pinfo("===\nThis PCI device is UNTESTED. Please report the 'flashrom -p "
					  "xxxx' output\n"
					  "to flashrom@flashrom.org if it works for you. Please add the name "
					  "of your\n"
					  "PCI device to the subject. Thank you for your help!\n===\n");

			/* FIXME: We should count all matching devices, not
			 * just those with a valid BAR.
			 */
			if ((addr = pcidev_readbar(dev, bar)) != 0) {
				found_dev = dev;
				found++;
			}
		}
	}

	/* Only continue if exactly one supported PCI dev has been found. */
	if (found == 0) {
		msg_perr("Error: No supported PCI device found.\n");
		return NULL;
	} else if (found > 1) {
		msg_perr("Error: Multiple supported PCI devices found. Use 'flashrom -p xxxx:pci=bb:dd.f'\n"
			 "to explicitly select the card with the given BDF (PCI bus, device, function).\n");
		return NULL;
	}

	return found_dev;
}

enum pci_write_type {
	pci_write_type_byte,
	pci_write_type_word,
	pci_write_type_long,
};

struct undo_pci_write_data {
	struct pci_dev *dev;
	int reg;
	enum pci_write_type type;
	union {
		uint8_t bytedata;
		uint16_t worddata;
		uint32_t longdata;
	};
};

int undo_pci_write(void *p)
{
	struct undo_pci_write_data *data = p;
	if (pacc == NULL || data->dev == NULL) {
		msg_perr("%s: Tried to undo PCI writes without a valid PCI %s!\n"
			"Please report a bug at flashrom@flashrom.org\n",
			__func__, data->dev == NULL ? "device" : "context");
		return 1;
	}
	msg_pdbg("Restoring PCI config space for %02x:%02x:%01x reg 0x%02x\n",
		 data->dev->bus, data->dev->dev, data->dev->func, data->reg);
	switch (data->type) {
	case pci_write_type_byte:
		pci_write_byte(data->dev, data->reg, data->bytedata);
		break;
	case pci_write_type_word:
		pci_write_word(data->dev, data->reg, data->worddata);
		break;
	case pci_write_type_long:
		pci_write_long(data->dev, data->reg, data->longdata);
		break;
	}
	/* p was allocated in register_undo_pci_write. */
	free(p);
	return 0;
}

#define register_undo_pci_write(a, b, c)				\
{									\
	struct undo_pci_write_data *undo_pci_write_data;		\
	undo_pci_write_data = malloc(sizeof(struct undo_pci_write_data)); \
	if (!undo_pci_write_data) {					\
		msg_gerr("Out of memory!\n");				\
		exit(1);						\
	}								\
	if (pacc)							\
		undo_pci_write_data->dev = pci_get_dev(pacc,		\
				a->domain, a->bus, a->dev, a->func);	\
	else								\
		undo_pci_write_data->dev =  NULL;			\
	undo_pci_write_data->reg = b;					\
	undo_pci_write_data->type = pci_write_type_##c;			\
	undo_pci_write_data->c##data = pci_read_##c(dev, reg);		\
	register_shutdown(undo_pci_write, undo_pci_write_data);		\
}

#define register_undo_pci_write_byte(a, b) register_undo_pci_write(a, b, byte)
#define register_undo_pci_write_word(a, b) register_undo_pci_write(a, b, word)
#define register_undo_pci_write_long(a, b) register_undo_pci_write(a, b, long)

int rpci_write_byte(struct pci_dev *dev, int reg, uint8_t data)
{
	register_undo_pci_write_byte(dev, reg);
	return pci_write_byte(dev, reg, data);
}

int rpci_write_word(struct pci_dev *dev, int reg, uint16_t data)
{
	register_undo_pci_write_word(dev, reg);
	return pci_write_word(dev, reg, data);
}

int rpci_write_long(struct pci_dev *dev, int reg, uint32_t data)
{
	register_undo_pci_write_long(dev, reg);
	return pci_write_long(dev, reg, data);
}

/*
 * This is reinvented pci device matching and access infrastructure which:
 * - allows users to define a private which could be device specific.
 *   This avoids doing multiple lookups, which massively helps the ati spi
 *   driver which will support hundreds of devices.
 * - uses linux sysfs pci infrastructure for enable/disable and mapping
 *   resources. This circumvents the security restrictions surrounding
 *   /dev/mem, but means that support for other operating systems still
 *   needs to be cobbled together.
 * - looks up device names through libpci, keeping us from maintaining a
 *   separate names list.
 *
 */

/*
 *
 */
int
flashrom_pci_mmio_map(struct flashrom_pci_device *device, int bar)
{
	char filename[1024];
	int fd;

	if ((bar < 0) || (bar > 5)) {
		msg_perr("%s: Invalid BAR provided: %d\n", __func__, bar);
		return EINVAL;
	}

	if (device->mmio) {
		if (device->pci->size[bar] != device->mmio_size) {
			msg_perr("%s: already mapped bar: 0x%lXbytes @ %p)\n",
				 __func__, device->mmio_size, device->mmio);
			return EALREADY;
		} else
			return 0;
	}

	snprintf(filename, sizeof(filename) - 1,
		 "%sresource%d", device->sysfs_path, bar);

	fd = open(filename, O_RDWR);
	if (fd == -1) {
		msg_perr("%s: failed to open %s: %s\n",
			 __func__, filename, strerror(errno));
		return errno;
	}

	device->mmio_size = device->pci->size[bar];

	device->mmio = mmap(NULL, device->mmio_size, PROT_WRITE | PROT_READ,
			    MAP_SHARED, fd, 0);
	if (device->mmio == MAP_FAILED) {
		msg_perr("%s: mapping %s failed: %s\n",
			 __func__, filename, strerror(errno));
		close(fd);
		device->mmio_size = 0;
		return errno;
	}

	close(fd);

	return 0;
}

/*
 *
 */
void
flashrom_pci_mmio_unmap(struct flashrom_pci_device *device)
{
	if (device->mmio && device->mmio_size &&
	    (device->mmio != MAP_FAILED))
		munmap((void *) device->mmio, device->mmio_size);
	device->mmio = NULL;
	device->mmio_size = 0;
}

/*
 *
 */
int
flashrom_pci_device_enable(struct flashrom_pci_device *device)
{
	char filename[1024];
	int fd, ret;
	char enable;

	if (device->enabled)
		return 0;

	snprintf(filename, sizeof(filename) - 1,
		 "%senable", device->sysfs_path);

	fd = open(filename, O_RDWR);
	if (fd == -1) {
		msg_perr("%s: failed to open %s: %s\n",
			 __func__, filename, strerror(errno));
		return errno;
	}

	ret = read(fd, &enable, 1);
	if (ret != 1) {
		close(fd);
		msg_perr("%s: failed to read %s: %s\n",
			 __func__, filename, strerror(errno));
		return errno;
	}

	if (enable == '0') {
		device->was_disabled = true;
		ret = write(fd, "1", 1);
		if (ret != 1) {
			close(fd);
			msg_perr("%s: failed to write %s: %s\n",
				 __func__, filename, strerror(errno));
			return errno;
		}
	} else if (enable != '1') {
		close(fd);
		msg_perr("%s: invalid value read from %s: %c\n",
			 __func__, filename, enable);
		return EINVAL;
	}

	close(fd);

	device->enabled = true;

	return 0;
}

/*
 *
 */
int
flashrom_pci_device_disable(struct flashrom_pci_device *device)
{
	char filename[1024];
	int fd, ret;
	char enable;

	/* We do not want to disable the device if we did not enable it */
	if (!device->enabled || !device->was_disabled)
		return 0;

	snprintf(filename, sizeof(filename) - 1,
		 "%senable", device->sysfs_path);

	fd = open(filename, O_RDWR);
	if (fd == -1) {
		msg_perr("%s: failed to open %s: %s\n",
			 __func__, filename, strerror(errno));
		return errno;
	}

	ret = read(fd, &enable, 1);
	if (ret != 1) {
		close(fd);
		msg_perr("%s: failed to read %s: %s\n",
			 __func__, filename, strerror(errno));
		return errno;
	}

	if (enable == '1') {
		ret = write(fd, "0", 1);
		if (ret != 1) {
			close(fd);
			msg_perr("%s: failed to write %s: %s\n",
				 __func__, filename, strerror(errno));
			return errno;
		}
	} else if (enable != '0') {
		close(fd);
		msg_perr("%s: invalid value read from %s: %c\n",
			 __func__, filename, enable);
		return EINVAL;
	}

	close(fd);

	device->enabled = false;

	return 0;
}


/*
 *
 */
static int
flashrom_pci_device_shutdown(void *data)
{
	struct flashrom_pci_device *device = data;

	if (!device->sysfs_path) {
		msg_perr("%s: Tried to cleanup an invalid pci_device!\n"
			 "Please report a bug at flashrom@flashrom.org\n", __func__);
		return 1;
	}

	flashrom_pci_mmio_unmap(device);

	flashrom_pci_device_disable(device);

	free(device->sysfs_path);
	device->sysfs_path = NULL;

	if (device->private_data)
		msg_perr("%s: device \"%s\"still has private data attached!\n"
			 "Please report a bug at flashrom@flashrom.org\n",
			 device->name, __func__);
	device->private = NULL;

	free(device->name);
	device->name = NULL;

	free(device);

	return 0;
}

/*
 *
 */
struct flashrom_pci_device *
flashrom_pci_init(const struct flashrom_pci_match *matches)
{
	struct flashrom_pci_device *device;
	struct pci_filter filter;
	struct pci_dev *dev, *found_dev = NULL;
	const struct flashrom_pci_match *found_match = NULL;
	char *pcidev_sbdf;
	char buffer[1024], *name = NULL;
	int i, found = 0;

	if (pci_init_common())
		return NULL;

	pci_filter_init(pacc, &filter);

	/* Filter by bb:dd.f (if supplied by the user). */
	pcidev_sbdf = extract_programmer_param("pci");
	if (pcidev_sbdf) {
		char *msg;

		if ((msg = pci_filter_parse_slot(&filter, pcidev_sbdf))) {
			msg_perr("Error: %s\n", msg);
			return NULL;
		}

		free(pcidev_sbdf);
	}

	for (dev = pacc->devices; dev; dev = dev->next)
		if (pci_filter_match(&filter, dev)) {
			for (i = 0; matches[i].vendor_id ; i++)
				if ((dev->vendor_id == matches[i].vendor_id) &&
				    (dev->device_id == matches[i].device_id))
					break;

			if (!matches[i].vendor_id)
				continue;


			name = pci_lookup_name(pacc, buffer, sizeof(buffer),
					       PCI_LOOKUP_VENDOR | PCI_LOOKUP_DEVICE,
					       dev->vendor_id, dev->device_id);
			if (name)
				msg_pinfo("Detected %04x:%04x@%02x:%02x.%x \"%s\"\n",
					  dev->vendor_id, dev->device_id,
					  dev->bus, dev->dev, dev->func, name);
			else
				msg_pinfo("Detected %04x:%04x@%02x:%02x.%x \"%s\"\n",
					  dev->vendor_id, dev->device_id,
					  dev->bus, dev->dev, dev->func,
					  "<unknown pciids>");

			if (matches[i].status == NT)
				msg_pinfo("===\nThis PCI device is UNTESTED. Please report the 'flashrom -p "
					  "xxxx' output\n"
					  "to flashrom@flashrom.org if it works for you. Please add the name "
					  "of your\n"
					  "PCI device to the subject. Thank you for your help!\n===\n");

			found_dev = dev;
			found_match = &matches[i];
			found++;
		}

	/* Only continue if exactly one supported PCI dev has been found. */
	if (!found) {
		msg_perr("Error: No supported PCI device found.\n");
		return NULL;
	} else if (found > 1) {
		msg_perr("Error: Multiple supported PCI devices found. Use 'flashrom -p xxxx:pci=bb:dd.f'\n"
			 "to explicitly select the card with the given BDF (PCI bus, device, function).\n");
		return NULL;
	}

	device = calloc(1, sizeof(*device));

	device->name = strdup(name);

	device->device_id = found_dev->device_id;
	device->vendor_id = found_dev->vendor_id;

	device->pci = found_dev;

	snprintf(buffer, sizeof(buffer) - 1,
		 "/sys/bus/pci/devices/%04x:%02x:%02x.%01x/",
		 found_dev->domain, found_dev->bus, found_dev->dev, found_dev->func);
	device->sysfs_path = strdup(buffer);

	device->private = found_match->private;

	register_shutdown(flashrom_pci_device_shutdown, device);

	if (flashrom_pci_device_enable(device))
		return NULL;

	return device;
}

/*
 *
 * Accessors of mmio.
 *
 * It is nonsensical to roll these back automatically.
 * Programmers are responsible for their own restauration.
 *
 */
uint8_t flashrom_pci_mmio_byte_read(struct flashrom_pci_device *device,
				    off_t address)
{
	if (address >= device->mmio_size) {
		errno = EFAULT;
		return -1;
	}

	return device->mmio[address];
}

void flashrom_pci_mmio_byte_write(struct flashrom_pci_device *device,
				  off_t address, uint8_t value)
{
	if (address >= device->mmio_size) {
		errno = EFAULT;
		return;
	}

	device->mmio[address] = value;
}

void flashrom_pci_mmio_byte_mask(struct flashrom_pci_device *device,
				 off_t address, uint8_t value, uint8_t mask)
{
	uint8_t temp;

	if (address >= device->mmio_size) {
		errno = EFAULT;
		return;
	}

	value &= mask;

	temp = device->mmio[address] & ~mask;
	temp |= value & mask;

	device->mmio[address] = temp;
}

uint32_t flashrom_pci_mmio_long_read(struct flashrom_pci_device *device,
				     off_t address)
{
	volatile uint32_t *mmio = (volatile uint32_t *) &device->mmio[address];

	if ((address >= device->mmio_size) || (address & 0x03)) {
		errno = EFAULT;
		return -1;
	}

	return mmio[0];
}

void flashrom_pci_mmio_long_write(struct flashrom_pci_device *device,
				  off_t address, uint32_t value)
{
	volatile uint32_t *mmio = (volatile uint32_t *) &device->mmio[address];

	if ((address >= device->mmio_size) || (address & 0x03)) {
		errno = EFAULT;
		return;
	}

	mmio[0] = value;
}

void flashrom_pci_mmio_long_mask(struct flashrom_pci_device *device,
				 off_t address, uint32_t value, uint32_t mask)
{
	volatile uint32_t *mmio = (volatile uint32_t *) &device->mmio[address];
	uint32_t temp;

	if ((address >= device->mmio_size) || (address & 0x03)) {
		errno = EFAULT;
		return;
	}

	value &= mask;

	temp = mmio[0] & ~mask;
	temp |= value & mask;

	mmio[0] = temp;
}
