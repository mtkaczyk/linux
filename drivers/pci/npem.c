// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe Enclosure management driver created for LED interfaces based on
 * indications. It says *what indications* blink but does not specify *how*
 * they blink - it is hardware defined.
 *
 * The driver name refers to Native PCIe Enclosure Management. It is
 * first indication oriented standard with specification.
 *
 * Native PCIe Enclosure Management (NPEM)
 *	PCIe Base Specification r6.1 sec 6.28
 *	PCIe Base Specification r6.1 sec 7.9.19
 *
 * Copyright (c) 2023-2024 Intel Corporation
 *	Mariusz Tkaczyk <mariusz.tkaczyk@linux.intel.com>
 */

#include <linux/acpi.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/iopoll.h>
#include <linux/leds.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/types.h>
#include <linux/uleds.h>

#include "pci.h"

struct indication {
	u32 bit;
	const char *name;
};

static const struct indication npem_indications[] = {
	{PCI_NPEM_IND_OK,	"enclosure:ok"},
	{PCI_NPEM_IND_LOCATE,	"enclosure:locate"},
	{PCI_NPEM_IND_FAIL,	"enclosure:fail"},
	{PCI_NPEM_IND_REBUILD,	"enclosure:rebuild"},
	{PCI_NPEM_IND_PFA,	"enclosure:pfa"},
	{PCI_NPEM_IND_HOTSPARE,	"enclosure:hotspare"},
	{PCI_NPEM_IND_ICA,	"enclosure:ica"},
	{PCI_NPEM_IND_IFA,	"enclosure:ifa"},
	{PCI_NPEM_IND_IDT,	"enclosure:idt"},
	{PCI_NPEM_IND_DISABLED,	"enclosure:disabled"},
	{PCI_NPEM_IND_SPEC_0,	"enclosure:specific_0"},
	{PCI_NPEM_IND_SPEC_1,	"enclosure:specific_1"},
	{PCI_NPEM_IND_SPEC_2,	"enclosure:specific_2"},
	{PCI_NPEM_IND_SPEC_3,	"enclosure:specific_3"},
	{PCI_NPEM_IND_SPEC_4,	"enclosure:specific_4"},
	{PCI_NPEM_IND_SPEC_5,	"enclosure:specific_5"},
	{PCI_NPEM_IND_SPEC_6,	"enclosure:specific_6"},
	{PCI_NPEM_IND_SPEC_7,	"enclosure:specific_7"},
	{0,			NULL}
};

#define for_each_indication(ind, inds) \
	for (ind = inds; ind->bit; ind++)

/*
 * The driver has internal list of supported indications. Ideally, the driver
 * should not touch bits that are not defined and for which LED devices are
 * not exposed but in reality, it needs to turn them off.
 *
 * Otherwise, there will be no possibility to turn off indications turned on by
 * other utilities or turned on by default and it leads to bad user experience.
 *
 * Additionally, it excludes NPEM commands like RESET or ENABLE.
 */
static u32 reg_to_indications(u32 caps, const struct indication *inds)
{
	const struct indication *ind;
	u32 supported_indications = 0;

	for_each_indication(ind, inds)
		supported_indications |= ind->bit;

	return caps & supported_indications;
}

/**
 * struct npem_led - LED details
 * @indication: indication details
 * @npem: npem device
 * @name: LED name
 * @led: LED device
 */
struct npem_led {
	const struct indication *indication;
	struct npem *npem;
	char name[LED_MAX_NAME_SIZE];
	struct led_classdev led;
};

/**
 * struct npem_ops - backend specific callbacks
 * @inds: supported indications array, set of indications is backend specific
 * @get_active_indications: get active indications
 *	npem: npem device
 *	inds: response buffer
 * @set_active_indications: set new indications
 *	npem: npem device
 *	inds: bit mask to set
 */
struct npem_ops {
	const struct indication *inds;
	int (*get_active_indications)(struct npem *npem, u32 *inds);
	int (*set_active_indications)(struct npem *npem, u32 inds);
};

/**
 * struct npem - NPEM device properties
 * @dev: PCIe device this driver is attached to
 * @ops: Backend specific callbacks
 * @lock: serialized accessing npem device from multiple LED devices
 * @pos: NPEM backed only, NPEM capability offset
 * @supported_indications: bit mask of supported indications
 *			   non-indication and reserved bits are cleared
 * @active_indications: bit mask of active indications
 *			non-indication and reserved bits are cleared
 * @active_inds_initialized: if set then active_indications are initialized
 * @led_cnt: Supported LEDs count
 * @leds: supported LEDs
 */
struct npem {
	struct pci_dev *dev;
	const struct npem_ops *ops;
	struct mutex lock;
	u16 pos;
	u32 supported_indications;
	u32 active_indications;

	/*
	 * Use lazy loading for active_indications to not play with initcalls.
	 * It is needed to allow _DSM initialization on DELL platforms, where
	 * ACPI_IPMI must be loaded first.
	 */
	unsigned int active_inds_initialized:1;

	int led_cnt;
	struct npem_led leds[];
};

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

static int npem_get_active_indications(struct npem *npem, u32 *inds)
{
	u32 ctrl;
	int ret;

	lockdep_assert_held(&npem->lock);

	ret = npem_read_reg(npem, PCI_NPEM_CTRL, &ctrl);
	if (ret)
		return ret;

	/* If PCI_NPEM_CTRL_ENABLE is not set then no indication should blink */
	if (!(ctrl & PCI_NPEM_CTRL_ENABLE)) {
		*inds = 0;
		return 0;
	}

	*inds = ctrl & npem->supported_indications;

	return 0;
}

static int npem_set_active_indications(struct npem *npem, u32 inds)
{
	int ctrl, ret, ret_val;
	u32 cc_status;

	lockdep_assert_held(&npem->lock);

	/* This bit is always required */
	ctrl = inds | PCI_NPEM_CTRL_ENABLE;

	ret = npem_write_ctrl(npem, ctrl);
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
	if (ret)
		return ret;
	if (ret_val)
		return ret_val;

	/*
	 * All writes to control register, including writes that do not change
	 * the register value, are NPEM commands and should eventually result
	 * in a command completion indication in the NPEM Status Register.
	 *
	 * PCIe Base Specification r6.1 sec 7.9.19.3
	 *
	 * Register may not be updated, or other conflicting bits may be
	 * cleared. Spec is not strict here. Read NPEM Control register after
	 * write to keep cache in-sync.
	 */
	return npem_get_active_indications(npem, &npem->active_indications);
}

static const struct npem_ops npem_ops = {
	.inds = npem_indications,
	.get_active_indications = npem_get_active_indications,
	.set_active_indications = npem_set_active_indications,
};

#define DSM_GUID GUID_INIT(0x5d524d9d, 0xfff9, 0x4d4b,  0x8c, 0xb7, 0x74, 0x7e,\
			   0xd5, 0x1e, 0x19, 0x4d)
#define GET_SUPPORTED_STATES_DSM	1
#define GET_STATE_DSM			2
#define SET_STATE_DSM			3

static const guid_t dsm_guid = DSM_GUID;

static bool npem_has_dsm(struct pci_dev *pdev)
{
	acpi_handle handle;

	handle = ACPI_HANDLE(&pdev->dev);
	if (!handle)
		return false;

	return acpi_check_dsm(handle, &dsm_guid, 0x1,
			      BIT(GET_SUPPORTED_STATES_DSM) |
			      BIT(GET_STATE_DSM) | BIT(SET_STATE_DSM));
}

static int npem_initialize_active_indications(struct npem *npem)
{
	int ret;

	lockdep_assert_held(&npem->lock);

	if (npem->active_inds_initialized)
		return 0;

	ret = npem->ops->get_active_indications(npem,
						&npem->active_indications);
	if (ret)
		return ret;

	npem->active_inds_initialized = true;
	return 0;
}

/*
 * The status of each indicator is cached on first brightness_ get/set time and
 * updated at write time.
 * brightness_get() is only responsible for reflecting the last written/cached
 * value.
 */
static enum led_brightness brightness_get(struct led_classdev *led)
{
	struct npem_led *nled = container_of(led, struct npem_led, led);
	struct npem *npem = nled->npem;
	int ret, val = 0;

	ret = mutex_lock_interruptible(&npem->lock);
	if (ret)
		return ret;

	ret = npem_initialize_active_indications(npem);
	if (ret)
		goto out;

	if (npem->active_indications & nled->indication->bit)
		val = 1;

out:
	mutex_unlock(&npem->lock);
	return val;
}

static int brightness_set(struct led_classdev *led,
			  enum led_brightness brightness)
{
	struct npem_led *nled = container_of(led, struct npem_led, led);
	struct npem *npem = nled->npem;
	u32 indications;
	int ret;

	ret = mutex_lock_interruptible(&npem->lock);
	if (ret)
		return ret;

	ret = npem_initialize_active_indications(npem);
	if (ret)
		goto out;

	if (brightness == 0)
		indications = npem->active_indications & ~(nled->indication->bit);
	else
		indications = npem->active_indications | nled->indication->bit;

	ret = npem->ops->set_active_indications(npem, indications);

out:
	mutex_unlock(&npem->lock);
	return ret;
}

static void npem_free(struct npem *npem)
{
	struct npem_led *nled;
	int cnt;

	if (!npem)
		return;

	for (cnt = 0; cnt < npem->led_cnt; cnt++) {
		nled = &npem->leds[cnt];

		if (nled->name[0])
			led_classdev_unregister(&nled->led);
	}

	mutex_destroy(&npem->lock);
	kfree(npem);
}

static int pci_npem_set_led_classdev(struct npem *npem, struct npem_led *nled)
{
	struct led_classdev *led = &nled->led;
	struct led_init_data init_data = {};
	char *name = nled->name;
	int ret;

	init_data.devicename = pci_name(npem->dev);
	init_data.default_label = nled->indication->name;

	ret = led_compose_name(&npem->dev->dev, &init_data, name);
	if (ret)
		return ret;

	led->name = name;
	led->brightness_set_blocking = brightness_set;
	led->brightness_get = brightness_get;
	led->max_brightness = 1;
	led->default_trigger = "none";
	led->flags = 0;

	ret = led_classdev_register(&npem->dev->dev, led);
	if (ret)
		/* Clear the name to indicate that it is not registered. */
		name[0] = 0;
	return ret;
}

static int pci_npem_init(struct pci_dev *dev, const struct npem_ops *ops,
			 int pos, u32 caps)
{
	u32 supported = reg_to_indications(caps, ops->inds);
	int supported_cnt = hweight32(supported);
	const struct indication *indication;
	struct npem_led *nled;
	struct npem *npem;
	int led_idx = 0;
	int ret;

	npem = kzalloc(struct_size(npem, leds, supported_cnt), GFP_KERNEL);
	if (!npem)
		return -ENOMEM;

	npem->supported_indications = supported;
	npem->led_cnt = supported_cnt;
	npem->pos = pos;
	npem->dev = dev;
	npem->ops = ops;

	mutex_init(&npem->lock);

	for_each_indication(indication, npem_indications) {
		if (!(npem->supported_indications & indication->bit))
			continue;

		nled = &npem->leds[led_idx++];
		nled->indication = indication;
		nled->npem = npem;

		ret = pci_npem_set_led_classdev(npem, nled);
		if (ret) {
			npem_free(npem);
			return ret;
		}
	}

	dev->npem = npem;
	return 0;
}

void pci_npem_remove(struct pci_dev *dev)
{
	npem_free(dev->npem);
}

void pci_npem_create(struct pci_dev *dev)
{
	const struct npem_ops *ops = &npem_ops;
	int pos = 0, ret;
	u32 cap;

	if (!npem_has_dsm(dev)) {
		pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_NPEM);
		if (pos == 0)
			return;

		if (pci_read_config_dword(dev, pos + PCI_NPEM_CAP, &cap) != 0 ||
		    (cap & PCI_NPEM_CAP_CAPABLE) == 0)
			return;
	} else {
		/*
		 * OS should use the DSM for LED control if it is available
		 * PCI Firmware Spec r3.3 sec 4.7.
		 */
		return;
	}

	ret = pci_npem_init(dev, ops, pos, cap);
	if (ret)
		pci_err(dev, "Failed to register PCIe Enclosure Management driver, err: %d\n",
			ret);
}
