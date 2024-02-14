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
#include <linux/errno.h>
#include <linux/iopoll.h>
#include <linux/leds.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/uleds.h>

#include "pci.h"

/**
 * struct npem_led - LED details.
 * @pattern: pattern details.
 * @npem: npem device.
 * @name: LED name.
 * @led: LED device.
 */
struct npem_led {
	const struct npem_pattern *pattern;
	struct npem *npem;
	char name[LED_MAX_NAME_SIZE];
	struct led_classdev led;
};

/**
 * struct npem - NPEM device properties.
 * @dev: PCIe device with NPEM capability.
 * @npem_lock: to keep concurrent updates synchronized.
 * @pos: capability offset.
 * @capabilities: capabilities register content.
 * @control: control register content.
 * @leds: available LEDs.
 */
struct npem {
	struct pci_dev *dev;
	struct mutex npem_lock;
	u16 pos;
	u32 capabilities;
	u32 control;
	struct npem_led leds[];
};

struct npem_pattern {
	u32 bit;
	char *name;
};

static const struct npem_pattern patterns[] = {
	{PCI_NPEM_OK,		"enclosure:ok"},
	{PCI_NPEM_LOCATE,	"enclosure:locate"},
	{PCI_NPEM_FAIL,		"enclosure:fail"},
	{PCI_NPEM_REBUILD,	"enclosure:rebuild"},
	{PCI_NPEM_PFA,		"enclosure:pfa"},
	{PCI_NPEM_HOTSPARE,	"enclosure:hotspare"},
	{PCI_NPEM_ICA,		"enclosure:ica"},
	{PCI_NPEM_IFA,		"enclosure:ifa"},
	{PCI_NPEM_IDT,		"enclosure:idt"},
	{PCI_NPEM_DISABLED,	"enclosure:disabled"},
	{PCI_NPEM_SPEC_0,	"enclosure:specific_0"},
	{PCI_NPEM_SPEC_1,	"enclosure:specific_1"},
	{PCI_NPEM_SPEC_2,	"enclosure:specific_2"},
	{PCI_NPEM_SPEC_3,	"enclosure:specific_3"},
	{PCI_NPEM_SPEC_4,	"enclosure:specific_4"},
	{PCI_NPEM_SPEC_5,	"enclosure:specific_5"},
	{PCI_NPEM_SPEC_6,	"enclosure:specific_6"},
	{PCI_NPEM_SPEC_7,	"enclosure:specific_7"},
};

#define for_each_npem_pattern(ptrn)\
	for (ptrn = patterns; ptrn < patterns + ARRAY_SIZE(patterns); ptrn++)

/* Reserved bits are outside specification, count only defined bits. */
static int npem_get_supported_patterns_count(u32 capabilities)
{
	const struct npem_pattern *pattern;
	int cnt = 0;

	for_each_npem_pattern(pattern)
		if (capabilities & pattern->bit)
			cnt++;

	return cnt;
}

static int npem_read_reg(struct npem *npem, u16 reg, u32 *val)
{
	int ret = pci_read_config_dword(npem->dev, npem->pos + reg, val);

	return pcibios_err_to_errno(ret);
}

static int npem_write_ctrl(struct npem *npem, u32 reg)
{
	int pos = npem->pos + PCI_NPEM_CTRL;
	int ret = pci_write_config_dword(npem->dev, pos, reg);

	return pcibios_err_to_errno(ret);
}

static int npem_set_active_patterns(struct npem *npem, u32 val)
{
	int ret, ret_val;
	u32 cc_status;

	lockdep_assert_held(&npem->npem_lock);

	/* This bit is always required */
	val |= PCI_NPEM_CTRL_ENABLED;
	ret = npem_write_ctrl(npem, val);
	if (ret)
		return ret;

	/*
	 * For the case where a NPEM command has not completed immediately,
	 * it is recommended that software not continuously “spin” on polling
	 * the status register, but rather poll under interrupt at a reduced
	 * rate; for example at 10 ms intervals.
	 *
	 * PCIe r6.1 sec 6.28 "Implementation Note: Software Polling of NPEM
	 * Command Completed"
	 */
	ret = read_poll_timeout(npem_read_reg, ret_val,
				ret_val || (cc_status & PCI_NPEM_STATUS_CC),
				10 * USEC_PER_MSEC, USEC_PER_SEC, false, npem,
				PCI_NPEM_STATUS, &cc_status);

	return ret ?: ret_val;
}

static enum led_brightness npem_get(struct led_classdev *led)
{
	struct npem_led *nled = container_of(led, struct npem_led, led);
	u32 mask = nled->pattern->bit | PCI_NPEM_CTRL_ENABLED;
	struct npem *npem = nled->npem;
	int ret, val = 0;

	ret = mutex_lock_interruptible(&npem->npem_lock);
	if (ret)
		return ret;

	/* Pattern is ON if pattern and PCI_NPEM_CTRL_ENABLED bits are set */
	if ((npem->control & mask) == mask)
		val = 1;

	mutex_unlock(&npem->npem_lock);

	return val;
}

int npem_set(struct led_classdev *led, enum led_brightness brightness)
{
	struct npem_led *nled = container_of(led, struct npem_led, led);
	struct npem *npem = nled->npem;
	u32 patterns;
	int ret;

	ret = mutex_lock_interruptible(&npem->npem_lock);
	if (ret)
		return ret;

	if (brightness == LED_OFF)
		patterns = npem->control & ~(nled->pattern->bit);
	else
		patterns = npem->control | nled->pattern->bit;

	ret = npem_set_active_patterns(npem, patterns);
	if (ret == 0)
		/*
		 * Read register after write to keep cache in-sync. Controller
		 * may modify active bits, e.g. some patterns could be mutally
		 * exclusive.
		 */
		ret = npem_read_reg(npem, PCI_NPEM_CTRL, &npem->control);

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

static int npem_set_led_classdev(struct npem *npem, struct npem_led *nled)
{
	struct led_classdev *led = &nled->led;
	struct led_init_data init_data = {};
	char *name = nled->name;
	int ret;

	init_data.devicename = pci_name(npem->dev);
	init_data.default_label = nled->pattern->name;

	ret = led_compose_name(&npem->dev->dev, &init_data, name);
	if (ret)
		return ret;

	led->name = name;
	led->brightness_set_blocking = npem_set;
	led->brightness_get = npem_get;
	led->max_brightness = LED_ON;
	led->default_trigger = "none";
	led->flags = 0;

	ret = led_classdev_register(&npem->dev->dev, led);
	if (ret)
		/* Clear the name to indicate that it is not registered. */
		name[0] = 0;
	return ret;
}

void npem_free(struct npem *npem, int led_cnt)
{
	struct npem_led *nled;
	int cnt = 0;

	while (cnt < led_cnt) {
		nled = &npem->leds[cnt++];

		if (nled->name[0])
			led_classdev_unregister(&nled->led);
	}

	mutex_destroy(&npem->npem_lock);
	kfree(npem);
}

int npem_init(struct pci_dev *dev, int pos, u32 capabilities)
{
	int led_cnt = npem_get_supported_patterns_count(capabilities);
	const struct npem_pattern *pattern;
	struct npem_led *nled;
	struct npem *npem;
	int led_idx = 0;
	int ret;

	npem = kzalloc(sizeof(struct npem) + sizeof(struct npem_led) * led_cnt,
		       GFP_KERNEL);
	if (!npem)
		return -ENOMEM;

	npem->pos = pos;
	npem->dev = dev;
	npem->capabilities = capabilities;

	ret = npem_read_reg(npem, PCI_NPEM_CTRL, &npem->control);
	if (ret) {
		npem_free(npem, led_cnt);
		return ret;
	}

	/*
	 * Do not take npem->npem_lock, get_brightness() is called on
	 * registration path.
	 */
	mutex_init(&npem->npem_lock);

	for_each_npem_pattern(pattern) {
		if ((npem->capabilities & pattern->bit) == 0)
			/* Do not register not supported pattern */
			continue;

		nled = &npem->leds[led_idx++];
		nled->pattern = pattern;
		nled->npem = npem;

		ret = npem_set_led_classdev(npem, nled);
		if (ret) {
			npem_free(npem, led_cnt);
			return ret;
		}
	}

	dev->npem = npem;
	return 0;
}

void pci_npem_remove(struct pci_dev *dev)
{
	npem_free(dev->npem,
		  npem_get_supported_patterns_count(dev->npem->capabilities));
}

void pci_npem_create(struct pci_dev *dev)
{
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

	ret = npem_init(dev, pos, cap);
	if (ret)
		pci_err(dev, "Failed to register Native PCIe Enclosure Management, err: %d\n",
			ret);
}
