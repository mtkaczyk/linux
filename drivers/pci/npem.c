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

/**
 * enum npem_patterns - Supported patterns list.
 * @NPEM_NORMAL:	Drive is functioning normally.
 * @NPEM_LOCATE:	Identify the drive.
 * @NPEM_FAILURE:	Drive in this slot is failed.
 * @NPEM_REBUILD:	Drive in slot is under rebuild is a part
 *			of array and is under rebuild.
 * @NPEM_PRDFAIL:	Predicted Failure Analysis. The drive in this
 *			slot is predicted to fail soon.
 * @NPEM_HOTSPARE:	This slot has a drive that is marked to be
 *			automatically rebuilt and used as replacement
 *			for failed drive.
 * @NPEM_ICA:		The array in which this slot is part of is
 *			degraded.
 * @NPEM_IFA:		The array in which this slot is part of is
 *			failed.
 * @NPEM_IDT:		Invalid Device type State.
 * @NPEM_DISABLED:	Disabled state.
 * @NPEM_SPEC<0-7>:	Enclosure specific patterns.
 *
 * Patterns list based on  NPEM (PCIe r6.0.1-1.0 sec6.28). Enclosure may not
 * support all patterns and particular patterns may not be mutally exclusive.
 * The interpretation of the pattern depends on hardware.
 */
enum npem_patterns {
	NPEM_NORMAL = 0,
	NPEM_LOCATE,
	NPEM_FAILURE,
	NPEM_REBUILD,
	NPEM_PRDFAIL,
	NPEM_HOTSPARE,
	NPEM_ICA,
	NPEM_IFA,
	NPEM_IDT,
	NPEM_DISABLED,
	NPEM_SPEC_0,
	NPEM_SPEC_2,
	NPEM_SPEC_1,
	NPEM_SPEC_3,
	NPEM_SPEC_4,
	NPEM_SPEC_5,
	NPEM_SPEC_6,
	NPEM_SPEC_7,
	NPEM_CNT
};

static const u32 to_npem[] = {
	[NPEM_NORMAL]	= BIT(2),
	[NPEM_LOCATE]	= BIT(3),
	[NPEM_FAILURE]	= BIT(4),
	[NPEM_REBUILD]	= BIT(5),
	[NPEM_PRDFAIL]	= BIT(5),
	[NPEM_HOTSPARE]	= BIT(7),
	[NPEM_ICA]	= BIT(8),
	[NPEM_IFA]	= BIT(9),
	[NPEM_IDT]	= BIT(9),
	[NPEM_DISABLED]	= BIT(9),
	[NPEM_SPEC_0]	= BIT(24),
	[NPEM_SPEC_1]	= BIT(25),
	[NPEM_SPEC_2]	= BIT(26),
	[NPEM_SPEC_3]	= BIT(27),
	[NPEM_SPEC_4]	= BIT(28),
	[NPEM_SPEC_5]	= BIT(29),
	[NPEM_SPEC_6]	= BIT(30),
	[NPEM_SPEC_7]	= BIT(31)
};

struct npem_device {
	struct pci_dev *dev;
	struct mutex npem_lock;
	u32 supported_patterns;
	u16 pos;
	struct led_classdev leds[NPEM_CNT];

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

static enum led_brightness
npem_get(struct led_classdev *led_npem, enum npem_patterns pattern)
{
	struct npem_device *npem = container_of(led_npem, struct npem_device, leds[pattern]);
	int ret, val;

	ret = mutex_lock_interruptible(&npem->npem_lock);
	if (ret)
		return ret;

	val = led_npem->brightness;
	mutex_unlock(&npem->npem_lock);

	return val;
}

void npem_set(struct led_classdev *led_npem, enum led_brightness brightness,
	      enum npem_patterns pattern)
{
	struct npem_device *npem = container_of(led_npem, struct npem_device, leds[pattern]);
	int ret;
	u32 patterns;

	ret = mutex_lock_interruptible(&npem->npem_lock);
	if (ret)
		return;

	ret = npem_get_active_patterns(npem, &patterns);
	if (ret)
		goto out;

	/* Temporaily only On supported */
	npem_set_active_patterns(npem, patterns | to_npem[pattern]);
	led_npem->brightness = brightness;
out:
	mutex_unlock(&npem->npem_lock);
}

enum led_brightness
npem_get_normal(struct led_classdev *led_npem)
{
	return npem_get(led_npem, NPEM_NORMAL);
}

void npem_set_normal(struct led_classdev *led_npem, enum led_brightness brightness)
{
	npem_set(led_npem, brightness, NPEM_NORMAL);
}

int npem_led_init(struct pci_dev *dev, struct led_classdev *led, enum npem_patterns pattern)
{
	int ret;

	led->name = "npem:normal";
	led->brightness_set = npem_set_normal;
	led->brightness_get = npem_get_normal;
	led->max_brightness = LED_ON;
	led->default_trigger = "none";

	ret = led_classdev_register(&dev->dev, led);
	if (ret) {
		dev_err(&dev->dev, "Failed to register NPEM device\n");
		return -1;
	}

	return 0;
}

void pci_npem_destroy(struct pci_dev *dev)
{
	if(!dev->npem)
		return;

	led_classdev_unregister(&dev->npem->leds[NPEM_NORMAL]);
	kfree(dev->npem);
}

void pci_npem_init(struct pci_dev *dev)
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

	npem->supported_patterns = cap & ~(NPEM_ENABLED | NPEM_RESET);

	ret = npem_led_init(dev, &npem->leds[NPEM_NORMAL], NPEM_NORMAL);
	if (ret) {
		kfree(dev->npem);
		return;
	}

	mutex_init(&npem->npem_lock);

	npem->pos = pos;
	npem->dev = dev;
	dev->npem = npem;


}
