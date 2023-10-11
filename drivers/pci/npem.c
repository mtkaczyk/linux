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
#include <linux/uleds.h>

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
 * Patterns list based on NPEM (PCIe r6.0.1-1.0 sec6.28). Enclosure may not
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
	NPEM_CNT /* Must be last */
};

struct npem_led_ops {
	char *name;
	u32 bit;
	enum led_brightness (*_get)(struct led_classdev *led_cdev);
	void (*_set)(struct led_classdev *led_cdev,
		     enum led_brightness brightness);
};

struct npem_device {
	struct pci_dev *dev;
	struct mutex npem_lock;
	u16 pos;
	u32 supported_patterns;
	u32 active_patterns;
	/* To track registration failures */
	int registered_patterns;
	struct led_classdev leds[NPEM_CNT];

};

int npem_set(struct led_classdev *led_npem, enum led_brightness brightness,
	     enum npem_patterns pattern);

static enum led_brightness
npem_get(struct led_classdev *led_npem, enum npem_patterns pattern);

#define LED_FUNCS(_enum, _name)					\
static void npem_set_##_name(struct led_classdev *led_npem,	\
			     enum led_brightness brightness)	\
{								\
	npem_set(led_npem, brightness, _enum);			\
}								\
static enum led_brightness					\
npem_get_##_name(struct led_classdev *led_npem)			\
{								\
	return npem_get(led_npem, _enum);			\
}								\

LED_FUNCS(NPEM_NORMAL, normal);
LED_FUNCS(NPEM_LOCATE, locate);
LED_FUNCS(NPEM_FAILURE, failure);
LED_FUNCS(NPEM_REBUILD, rebuild);
LED_FUNCS(NPEM_PRDFAIL, prdfail);
LED_FUNCS(NPEM_HOTSPARE, hotspare);
LED_FUNCS(NPEM_ICA, ica);
LED_FUNCS(NPEM_IFA, ifa);
LED_FUNCS(NPEM_IDT, itd);
LED_FUNCS(NPEM_DISABLED, disabled);
LED_FUNCS(NPEM_SPEC_0, enclosure_specific_0);
LED_FUNCS(NPEM_SPEC_1, enclosure_specific_1);
LED_FUNCS(NPEM_SPEC_2, enclosure_specific_2);
LED_FUNCS(NPEM_SPEC_3, enclosure_specific_3);
LED_FUNCS(NPEM_SPEC_4, enclosure_specific_4);
LED_FUNCS(NPEM_SPEC_5, enclosure_specific_5);
LED_FUNCS(NPEM_SPEC_6, enclosure_specific_6);
LED_FUNCS(NPEM_SPEC_7, enclosure_specific_7);

#define NPEM_OP(_enum, _bit, _name)		\
	[_enum] = {				\
		.name = "npem:" #_name,		\
		.bit = _bit,			\
		._get = npem_get_##_name,	\
		._set = npem_set_##_name,	\
	}

static const struct npem_led_ops ops[] = {
	NPEM_OP(NPEM_NORMAL, BIT(2), normal),
	NPEM_OP(NPEM_LOCATE, BIT(3), locate),
	NPEM_OP(NPEM_FAILURE, BIT(4), failure),
	NPEM_OP(NPEM_REBUILD, BIT(5), rebuild),
	NPEM_OP(NPEM_PRDFAIL, BIT(6), prdfail),
	NPEM_OP(NPEM_HOTSPARE, BIT(7), hotspare),
	NPEM_OP(NPEM_ICA, BIT(8), ica),
	NPEM_OP(NPEM_IFA, BIT(9), ifa),
	NPEM_OP(NPEM_IDT, BIT(10), itd),
	NPEM_OP(NPEM_DISABLED, BIT(11), disabled),
	NPEM_OP(NPEM_SPEC_0, BIT(24), enclosure_specific_0),
	NPEM_OP(NPEM_SPEC_1, BIT(25), enclosure_specific_1),
	NPEM_OP(NPEM_SPEC_2, BIT(26), enclosure_specific_2),
	NPEM_OP(NPEM_SPEC_3, BIT(27), enclosure_specific_3),
	NPEM_OP(NPEM_SPEC_4, BIT(28), enclosure_specific_4),
	NPEM_OP(NPEM_SPEC_5, BIT(29), enclosure_specific_5),
	NPEM_OP(NPEM_SPEC_6, BIT(30), enclosure_specific_6),
	NPEM_OP(NPEM_SPEC_7, BIT(31), enclosure_specific_7),
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
				ret_val || (cc_status & NPEM_CC), 15,
				USEC_PER_SEC, false, npem, PCI_NPEM_STATUS,
				&cc_status);

	return ret ?: ret_val;
}

int npem_set(struct led_classdev *led_npem, enum led_brightness brightness,
	      enum npem_patterns pattern)
{
	struct npem_device *npem = container_of(led_npem, struct npem_device,
						leds[pattern]);
	const struct npem_led_ops *op = &ops[pattern];
	int ret;
	u32 patterns;

	ret = mutex_lock_interruptible(&npem->npem_lock);
	if (ret)
		return ret;

	if (brightness == LED_OFF)
		patterns = npem->active_patterns & ~(op->bit);
	else
		patterns = npem->active_patterns | op->bit;

	ret = npem_set_active_patterns(npem, patterns);
	if (!ret)
		/*
		 * Read register after write to keep cache in-sync. Controller
		 * may modify active bits, e.g. some patterns could be mutally
		 * exclusive.
		 */
		npem_read_reg(npem, PCI_NPEM_CTRL, &npem->active_patterns);

	mutex_unlock(&npem->npem_lock);
	return ret;
}

static enum led_brightness
npem_get(struct led_classdev *led_npem, enum npem_patterns pattern)
{
	struct npem_device *npem = container_of(led_npem, struct npem_device,
						leds[pattern]);
	const struct npem_led_ops *op = &ops[pattern];
	int ret, val = 0;

	ret = mutex_lock_interruptible(&npem->npem_lock);
	if (ret)
		return ret;

	if (npem->active_patterns & op->bit)
		val = 1;

	mutex_unlock(&npem->npem_lock);

	return val;
}


int npem_leds_init(struct npem_device *npem)
{
	const struct npem_led_ops *op;
	struct pci_dev *dev = npem->dev;
	struct led_classdev *led;
	int ret;
	enum npem_patterns pattern;
	char *name;

	/*
	 * Do not take npem->npem_lock, get_brightness() is called on
	 * registration path.
	 */
	lockdep_assert_not_held(&npem->npem_lock);

	for (pattern = NPEM_NORMAL; pattern < NPEM_CNT; pattern++) {
		led = &npem->leds[pattern];
		op = &ops[pattern];

		name = kzalloc(LED_MAX_NAME_SIZE, GFP_KERNEL);
		if (!name) {
			led->name = NULL;
			return -ENOMEM;
		}

		ret = snprintf(name, LED_MAX_NAME_SIZE, "%d:%s", dev->devfn,
			       op->name);
		if (ret < 0)
			return ret;

		led->name = name;
		led->brightness_set = op->_set;
		led->brightness_get = op->_get;
		led->max_brightness = LED_ON;
		led->default_trigger = "none";
		led->flags = 0;

		if ((npem->supported_patterns & op->bit) == 0)
			led->flags |= LED_SYSFS_DISABLE;

		ret = led_classdev_register(&dev->dev, led);
		if (ret) {
			dev_err(&dev->dev, "Failed to register NPEM %s LED device: %d, aborting \n",
				op->name, ret);
			return ret;
		}
		npem->registered_patterns |= op->bit;
	}

	return 0;
}

void pci_npem_destroy(struct pci_dev *dev)
{
	struct npem_device *npem = dev->npem;
	const struct npem_led_ops *op;
	struct led_classdev *led;
	enum npem_patterns pattern;

	if(!npem)
		return;

	for (pattern = NPEM_NORMAL; pattern < NPEM_CNT; pattern++) {
		op = &ops[pattern];
		led = &npem->leds[pattern];

		if (npem->registered_patterns & op->bit)
			led_classdev_unregister(&dev->npem->leds[NPEM_NORMAL]);
		if(led->name)
			kfree(led->name);
	}

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
	mutex_init(&npem->npem_lock);

	npem->dev = dev;
	npem->pos = pos;
	npem->registered_patterns = 0;
	npem->supported_patterns = cap;

	ret = npem_read_reg(npem, PCI_NPEM_CTRL, &npem->active_patterns);
	if (ret) {
		kfree(dev->npem);
		return;
	}

	ret = npem_leds_init(npem);
	if (ret) {
		pci_npem_destroy(dev);
		return;
	}

	dev->npem = npem;
}
