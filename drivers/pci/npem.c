// SPDX-License-Identifier: GPL-2.0
/*
 * Native PCIe Enclosure Management
 *	PCIe Base Specification r6.1-1.0 sec 6.28
 *	PCIe Base Specification r6.1-1.0 sec 7.9.19
 *
 * Copyright (c) 2023 Intel Corporation
 *	Mariusz Tkaczyk <mariusz.tkaczyk@linux.intel.com>
 */
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/leds.h>
#include <linux/errno.h>
#include <linux/kstrtox.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/sysfs.h>
#include <linux/types.h>

#include "pci.h"

/* NPEM Registers */
#define PCI_NPEM_CAP		0x04
#define PCI_NPEM_CTRL		0x08
#define PCI_NPEM_STATUS		0x0c

/* Special NPEM bits. */
#define	NPEM_ENABLED	BIT(0)
#define	NPEM_RESET	BIT(1)
/* NPEM Command completed */
#define	NPEM_CC		BIT(0)

struct npem_device {
	struct pci_dev *dev;
	struct mutex npem_lock;
	u32 supported_patterns;
	u16 pos;
	struct led_classdev npem_normal;

};

static int npem_read_reg(struct npem_device *npem, u16 reg, u32 *val)
{
	int pos = npem->pos + reg;
	int ret = pci_read_config_dword(npem->dev, pos, val);

	return pcibios_err_to_errno(ret);
}

static int npem_write_ctrl(struct npem_device *npem, u32 reg)
{
	int pos = npem->pos + PCI_NPEM_CTRL;
	int ret = pci_write_config_dword(npem->dev, pos, reg);

	return pcibios_err_to_errno(ret);
}

static int npem_set_active_patterns(struct npem_device *npem, u32 val)
{
	u32 cc_status;
	int ret, ret_val;

	lockdep_assert_held(&npem->npem_lock);

	val |= NPEM_ENABLED;
	ret = npem_write_ctrl(npem, val);
	if (ret)
		return ret;

	/*
	 * If the status bit is not set within 1 second limit on command
	 * execution, software is permitted to repeat the NPEM command or issue
	 * the next NPEM command.
	 *
	 * For the case where an NPEM command has not completed when software
	 * polls the bit, it is recommended that software not continuously
	 * “spin” on polling the bit, but rather poll under interrupt
	 * at a reduced rate; for example at 10 ms intervals.
	 */
	ret = read_poll_timeout(npem_read_reg, ret_val,
				ret_val || (cc_status & NPEM_CC), 15, 1000000,
				false, npem, PCI_NPEM_STATUS, &cc_status);

	return ret ?: ret_val;
}

static int npem_get_active_patterns(struct npem_device *npem, u32 *output)
{
	int ret;

	lockdep_assert_held(&npem->npem_lock);

	ret = npem_read_reg(npem, PCI_NPEM_CTRL, output);
	if (ret != 0)
		return ret;

	*output &= ~(NPEM_ENABLED | NPEM_RESET);
	return 0;
}

enum led_brightness npem_normal_led_get(struct led_classdev *led_npem)
{
	struct npem_device *npem = container_of(led_npem, struct npem_device, npem_normal);
	int ret, val;

	ret = mutex_lock_interruptible(&npem->npem_lock);
	if (ret)
		return ret;

	val = led_npem->brightness;
	mutex_unlock(&npem->npem_lock);

	return val;
}

void npem_normal_led_set(struct led_classdev *led_npem,
			 enum led_brightness brightness)
{
	struct npem_device *npem = container_of(led_npem, struct npem_device, npem_normal);
	int ret;
	u32 patterns;

	if (brightness != LED_OFF && brightness != LED_ON)
		return;

	ret = mutex_lock_interruptible(&npem->npem_lock);
	if (ret)
		return;

	ret = npem_get_active_patterns(npem, &patterns);
	if (ret)
		goto out;

	npem_set_active_patterns(npem, patterns & 2);
out:
	mutex_unlock(&npem->npem_lock);
}

void pcie_npem_destroy(struct pci_dev *dev)
{
	if(!dev->npem)
		return;

	led_classdev_unregister(&dev->npem->npem_normal);
	kfree(dev->npem);
}

void pcie_npem_init(struct pci_dev *dev)
{
	struct npem_device *npem;
	int pos, ret;
	u32 cap;

	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_NPEM);
	if (pos == 0)
		return;

	if (pci_read_config_dword(dev, pos + PCI_NPEM_CAP, &cap) != 0 ||
	    (cap & NPEM_ENABLED) == 0)
		return;

	npem = kzalloc(sizeof(*npem), GFP_KERNEL);
	if (!npem)
		return;

	npem->npem_normal.name = "npem:normal";
	npem->npem_normal.brightness_set = npem_normal_led_set,
	npem->npem_normal.brightness_get = npem_normal_led_get,
	npem->npem_normal.max_brightness = LED_ON;

	npem->pos = pos;
	npem->supported_patterns = cap & ~(NPEM_ENABLED | NPEM_RESET);
	npem->dev = dev;
	dev->npem = npem;

	dev_info(&dev->dev, "Registering NPEM LED device\n");
	ret = led_classdev_register(&dev->dev, &dev->npem->npem_normal);
	if (ret) {
		dev_err(&dev->dev, "Failed to register NPEM device\n");
		kfree(npem);
		return;
	}
	mutex_init(&npem->npem_lock);
}
