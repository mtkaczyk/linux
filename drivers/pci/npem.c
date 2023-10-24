// SPDX-License-Identifier: GPL-2.0
/*
 * Native PCIe Enclosure Management
 *	PCIe Base Specification r6.1 sec 6.28
 *	PCIe Base Specification r6.1 sec 7.9.19
 *
 * Copyright (c) 2023 Intel Corporation
 *	Mariusz Tkaczyk <mariusz.tkaczyk@linux.intel.com>
 */
#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/iopoll.h>
#include <linux/leds.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/uleds.h>

#include "pci.h"

enum npem_patterns {
	NPEM_OK = 0,
	NPEM_LOCATE,
	NPEM_FAIL,
	NPEM_REBUILD,
	NPEM_PFA,
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
	int (*_set)(struct led_classdev *led_cdev,
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

static enum led_brightness
npem_get(struct led_classdev *led_npem, enum npem_patterns pattern);

static int
npem_set(struct led_classdev *led_npem, enum led_brightness brightness,
	 enum npem_patterns pattern);

#define LED_FUNCS(_enum, _name)					\
static enum led_brightness					\
npem_get_##_name(struct led_classdev *led_npem)			\
{								\
	return npem_get(led_npem, _enum);			\
}								\
static int npem_set_##_name(struct led_classdev *led_npem,	\
			     enum led_brightness brightness)	\
{								\
	return npem_set(led_npem, brightness, _enum);		\
}								\

LED_FUNCS(NPEM_OK, ok);
LED_FUNCS(NPEM_LOCATE, locate);
LED_FUNCS(NPEM_FAIL, fail);
LED_FUNCS(NPEM_REBUILD, rebuild);
LED_FUNCS(NPEM_PFA, pfa);
LED_FUNCS(NPEM_HOTSPARE, hotspare);
LED_FUNCS(NPEM_ICA, ica);
LED_FUNCS(NPEM_IFA, ifa);
LED_FUNCS(NPEM_IDT, idt);
LED_FUNCS(NPEM_DISABLED, disabled);
LED_FUNCS(NPEM_SPEC_0, specific_0);
LED_FUNCS(NPEM_SPEC_1, specific_1);
LED_FUNCS(NPEM_SPEC_2, specific_2);
LED_FUNCS(NPEM_SPEC_3, specific_3);
LED_FUNCS(NPEM_SPEC_4, specific_4);
LED_FUNCS(NPEM_SPEC_5, specific_5);
LED_FUNCS(NPEM_SPEC_6, specific_6);
LED_FUNCS(NPEM_SPEC_7, specific_7);

#define NPEM_OP(_enum, _bit, _name)		\
	[_enum] = {				\
		.name = #_name,			\
		.bit = _bit,			\
		._get = npem_get_##_name,	\
		._set = npem_set_##_name,	\
	}

static const struct npem_led_ops ops[] = {
	NPEM_OP(NPEM_OK, PCI_NPEM_OK, ok),
	NPEM_OP(NPEM_LOCATE, PCI_NPEM_LOCATE, locate),
	NPEM_OP(NPEM_FAIL, PCI_NPEM_FAIL, fail),
	NPEM_OP(NPEM_REBUILD, PCI_NPEM_REBUILD, rebuild),
	NPEM_OP(NPEM_PFA, PCI_NPEM_PFA, pfa),
	NPEM_OP(NPEM_HOTSPARE, PCI_NPEM_HOTSPARE, hotspare),
	NPEM_OP(NPEM_ICA, PCI_NPEM_ICA, ica),
	NPEM_OP(NPEM_IFA, PCI_NPEM_IFA, ifa),
	NPEM_OP(NPEM_IDT, PCI_NPEM_IDT, idt),
	NPEM_OP(NPEM_DISABLED, PCI_NPEM_DISABLED, disabled),
	NPEM_OP(NPEM_SPEC_0, PCI_NPEM_SPEC_0, specific_0),
	NPEM_OP(NPEM_SPEC_1, PCI_NPEM_SPEC_1, specific_1),
	NPEM_OP(NPEM_SPEC_2, PCI_NPEM_SPEC_2, specific_2),
	NPEM_OP(NPEM_SPEC_3, PCI_NPEM_SPEC_3, specific_3),
	NPEM_OP(NPEM_SPEC_4, PCI_NPEM_SPEC_4, specific_4),
	NPEM_OP(NPEM_SPEC_5, PCI_NPEM_SPEC_5, specific_5),
	NPEM_OP(NPEM_SPEC_6, PCI_NPEM_SPEC_6, specific_6),
	NPEM_OP(NPEM_SPEC_7, PCI_NPEM_SPEC_7, specific_7),
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
	int ret, ret_val;
	u32 cc_status;

	lockdep_assert_held(&npem->npem_lock);

	val |= PCI_NPEM_CTRL_ENABLED;
	ret = npem_write_ctrl(npem, val);
	if (ret)
		return ret;

	/*
	 * For the case where a NPEM command has not completed immediately,
	 * it is recommended that software not continuously “spin” on polling
	 * the status register, but rather poll under interrupt at a reduced
	 * rate; for example at 10 ms intervals.
	 */
	ret = read_poll_timeout(npem_read_reg, ret_val,
				ret_val || (cc_status & PCI_NPEM_STATUS_CC), 15,
				USEC_PER_SEC, false, npem, PCI_NPEM_STATUS,
				&cc_status);

	return ret ?: ret_val;
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

int npem_set(struct led_classdev *led_npem, enum led_brightness brightness,
	      enum npem_patterns pattern)
{
	struct npem_device *npem = container_of(led_npem, struct npem_device,
						leds[pattern]);
	const struct npem_led_ops *op = &ops[pattern];
	u32 patterns;
	int ret;

	ret = mutex_lock_interruptible(&npem->npem_lock);
	if (ret)
		return ret;

	if (brightness == LED_OFF)
		patterns = npem->active_patterns & ~(op->bit);
	else
		patterns = npem->active_patterns | op->bit;

	ret = npem_set_active_patterns(npem, patterns);
	if (ret == 0)
		/*
		 * Read register after write to keep cache in-sync. Controller
		 * may modify active bits, e.g. some patterns could be mutally
		 * exclusive.
		 */
		npem_read_reg(npem, PCI_NPEM_CTRL, &npem->active_patterns);

	mutex_unlock(&npem->npem_lock);
	return ret;
}

#define DSM_GUID GUID_INIT(0x5d524d9d, 0xfff9, 0x4d4b,  0x8c, 0xb7, 0x74, 0x7e,\
			   0xd5, 0x1e, 0x19, 0x4d)
#define GET_SUPPORTED_STATES_DSM	BIT(1)
#define GET_STATE_DSM			BIT(2)
#define SET_STATE_DSM			BIT(3)

static bool npem_has_dsm(struct pci_dev *pdev)
{
	static const guid_t dsm_guid = DSM_GUID;
	acpi_handle handle;

	handle = ACPI_HANDLE(&pdev->dev);
	if (!handle)
		return false;

	if (acpi_check_dsm(handle, &dsm_guid, 0x1, GET_SUPPORTED_STATES_DSM |
			   GET_STATE_DSM | SET_STATE_DSM) == true)
		return true;
	return false;
}

int npem_leds_init(struct npem_device *npem)
{
	struct pci_dev *dev = npem->dev;
	const struct npem_led_ops *op;
	enum npem_patterns pattern;
	struct led_classdev *led;
	char *name;
	int ret;

	/*
	 * Do not take npem->npem_lock, get_brightness() is called on
	 * registration path.
	 */
	lockdep_assert_not_held(&npem->npem_lock);

	for (pattern = NPEM_OK; pattern < NPEM_CNT; pattern++) {
		led = &npem->leds[pattern];
		op = &ops[pattern];

		name = kzalloc(LED_MAX_NAME_SIZE, GFP_KERNEL);
		if (!name) {
			led->name = NULL;
			return -ENOMEM;
		}

		ret = snprintf(name, LED_MAX_NAME_SIZE, "%s:enclosure:%s",
			       pci_name(dev), op->name);
		if (ret < 0)
			return ret;

		led->name = name;
		led->brightness_set_blocking = op->_set;
		led->brightness_get = op->_get;
		led->max_brightness = LED_ON;
		led->default_trigger = "none";
		led->flags = 0;

		if ((npem->supported_patterns & op->bit) == 0)
			led->flags |= LED_SYSFS_DISABLE;

		ret = led_classdev_register(&dev->dev, led);
		if (ret) {
			dev_err(&dev->dev, "Failed to register NPEM %s LED device: %d, aborting.\n",
				op->name, ret);
			return ret;
		}
		npem->registered_patterns |= op->bit;
	}

	return 0;
}

void pci_npem_remove(struct pci_dev *dev)
{
	struct npem_device *npem = dev->npem;
	const struct npem_led_ops *op;
	enum npem_patterns pattern;
	struct led_classdev *led;

	if (!npem)
		return;

	for (pattern = NPEM_OK; pattern < NPEM_CNT; pattern++) {
		op = &ops[pattern];
		led = &npem->leds[pattern];

		if (npem->registered_patterns & op->bit)
			led_classdev_unregister(&dev->npem->leds[pattern]);
		kfree(led->name);
	}

	mutex_destroy(&npem->npem_lock);
	kfree(dev->npem);
}

void pci_npem_create(struct pci_dev *dev)
{
	struct npem_device *npem;
	int pos, ret;
	u32 cap;

	if (!pci_is_pcie(dev))
		return;

	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_NPEM);
	if (pos == 0)
		return;

	if (pci_read_config_dword(dev, pos + PCI_NPEM_CAP, &cap) != 0 ||
	    (cap & PCI_NPEM_CAPABLE) == 0)
		return;

	/*
	 * OS should use the DSM for LED control if it is available
	 * PCI Firmware Spec r3.3 sec 4.7.
	 */
	if (npem_has_dsm(dev))
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
		pci_npem_remove(dev);
		return;
	}

	dev->npem = npem;
}
