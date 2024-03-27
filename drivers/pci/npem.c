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
 * _DSM Definitions for PCIe SSD Status LED
 *	 PCI Firmware Specification, r3.3 sec 4.7
 *
 * Two backends are supported to manipulate indications:  Direct NPEM register
 * access (npem_ops) and indirect access through the ACPI _DSM (dsm_ops).
 * _DSM is used if supported, else NPEM.
 *
 * Copyright (c) 2021-2022 Dell Inc.
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

/* _DSM PCIe SSD LED States are corresponding to NPEM register values */
static const struct indication dsm_indications[] = {
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
	{0,			NULL}
};

#define for_each_indication(ind, inds) \
	for (ind = inds; ind->bit; ind++)

/* To avoid confusion, do not keep any special bits in indications */
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
 * @inds: supported indications array
 * @get_active_indications: get active indications
 *	@npem: npem device
 *	@buf: response buffer
 * @set_active_indications: set new indications
 *	@npem: npem device
 *	@val: bit mask to set
 *
 * Handle communication with hardware. set_active_indications updates
 * npem->active_indications.
 */
struct npem_ops {
	const struct indication *inds;
	int (*get_active_indications)(struct npem *npem, u32 *buf);
	int (*set_active_indications)(struct npem *npem, u32 val);
};

/**
 * struct npem - NPEM device properties
 * @dev: PCIe device this driver is attached to
 * @ops: Backend specific callbacks
 * @npem_lock: to keep concurrent updates serialized
 * @pos: NPEM capability offset (only relevant for NPEM direct register access,
 *	 not DSM access method)
 * @supported_indications: bit mask of supported indications
 *			   non-indication and reserved bits are cleared
 * @active_indications: bit mask of active indications
 *			non-indication and reserved bits are cleared
 * @led_cnt: Supported LEDs count
 * @leds: supported LEDs
 */
struct npem {
	struct pci_dev *dev;
	const struct npem_ops *ops;
	struct mutex npem_lock;
	u16 pos;
	u32 supported_indications;
	u32 active_indications;
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

static int npem_get_active_indications(struct npem *npem, u32 *buf)
{
	int ret;

	ret = npem_read_reg(npem, PCI_NPEM_CTRL, buf);
	if (ret)
		return ret;

	/* If PCI_NPEM_CTRL_ENABLE is not set then no indication should blink */
	if (!(*buf & PCI_NPEM_CTRL_ENABLE))
		*buf = 0;

	/* Filter out not supported indications in response */
	*buf &= npem->supported_indications;
	return 0;
}

static int npem_set_active_indications(struct npem *npem, u32 val)
{
	int ret, ret_val;
	u32 cc_status;

	lockdep_assert_held(&npem->npem_lock);

	/* This bit is always required */
	val |= PCI_NPEM_CTRL_ENABLE;
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
	if (ret)
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

const struct npem_ops npem_ops = {
	.inds = npem_indications,
	.get_active_indications = npem_get_active_indications,
	.set_active_indications = npem_set_active_indications,
};

#define DSM_GUID GUID_INIT(0x5d524d9d, 0xfff9, 0x4d4b,  0x8c, 0xb7, 0x74, 0x7e,\
			   0xd5, 0x1e, 0x19, 0x4d)
#define GET_SUPPORTED_STATES_DSM	BIT(1)
#define GET_STATE_DSM			BIT(2)
#define SET_STATE_DSM			BIT(3)

static const guid_t dsm_guid = DSM_GUID;

static bool npem_has_dsm(struct pci_dev *pdev)
{
	acpi_handle handle;

	handle = ACPI_HANDLE(&pdev->dev);
	if (!handle)
		return false;

	return acpi_check_dsm(handle, &dsm_guid, 0x1, GET_SUPPORTED_STATES_DSM |
			      GET_STATE_DSM | SET_STATE_DSM);
}

struct dsm_output {
	u16 status;
	u8 function_specific_err;
	u8 vendor_specific_err;
	u32 state;
} __packed;

/**
 * dsm_evaluate() - send DSM PCIe SSD Status LED command
 * @pdev: PCI device
 * @dsm_func: DSM LED Function
 * @output: buffer to copy DSM Response
 * @value_to_set: value for SET_STATE_DSM function
 *
 * To not bother caller with ACPI context, the returned _DSM Output Buffer is
 * copied.
 */
static int dsm_evaluate(struct pci_dev *pdev, u64 dsm_func,
			struct dsm_output *output, u32 value_to_set)
{
	acpi_handle handle = ACPI_HANDLE(&pdev->dev);
	union acpi_object *out_obj, arg3[2];
	union acpi_object *arg3_p = NULL;

	if (dsm_func == SET_STATE_DSM) {
		arg3[0].type = ACPI_TYPE_PACKAGE;
		arg3[0].package.count = 1;
		arg3[0].package.elements = &arg3[1];

		arg3[1].type = ACPI_TYPE_BUFFER;
		arg3[1].buffer.length = 4;
		arg3[1].buffer.pointer = (u8 *)&value_to_set;

		arg3_p = arg3;
	}

	out_obj = acpi_evaluate_dsm_typed(handle, &dsm_guid, 0x1, dsm_func,
					  arg3_p, ACPI_TYPE_BUFFER);
	if (!out_obj)
		return -EIO;

	if (out_obj->buffer.length < sizeof(struct dsm_output)) {
		ACPI_FREE(out_obj);
		return -EIO;
	}

	memcpy(output, out_obj->buffer.pointer, sizeof(struct dsm_output));

	ACPI_FREE(out_obj);
	return 0;
}

static int dsm_get(struct pci_dev *pdev, u64 dsm_func, u32 *buf)
{
	struct dsm_output output;
	int ret = dsm_evaluate(pdev, dsm_func, &output, 0);

	if (ret)
		return ret;

	if (output.status != 0)
		return -EIO;

	*buf = output.state;
	return 0;
}

static int dsm_get_active_indications(struct npem *npem, u32 *buf)
{
	int ret = dsm_get(npem->dev, GET_STATE_DSM, buf);

	/* Filter out not supported indications in response */
	*buf &= npem->supported_indications;
	return ret;
}

static int dsm_set_active_indications(struct npem *npem, u32 value)
{
	struct dsm_output output;
	int ret = dsm_evaluate(npem->dev, SET_STATE_DSM, &output, value);

	if (ret)
		return ret;

	switch (output.status) {
	case 4:
		/*
		 * Not all bits are set. If this bit is set, the platform
		 * disregarded some or all of the request state changes. OSPM
		 * should check the resulting PCIe SSD Status LED States to see
		 * what, if anything, has changed.
		 *
		 * PCI Firmware Specification, r3.3 Table 4-19.
		 */
		if (output.function_specific_err != 1)
			return -EIO;
		fallthrough;
	case 0:
		break;
	default:
		return -EIO;
	}

	npem->active_indications = output.state;

	return 0;
}

const struct npem_ops dsm_ops = {
	.inds = dsm_indications,
	.get_active_indications = dsm_get_active_indications,
	.set_active_indications = dsm_set_active_indications,
};

/*
 * This function does not use ops->get_active_indications().
 * It returns cached value, npem->npem_lock is held and it is safe.
 */
static enum led_brightness brightness_get(struct led_classdev *led)
{
	struct npem_led *nled = container_of(led, struct npem_led, led);
	struct npem *npem = nled->npem;
	int ret, val = LED_OFF;

	ret = mutex_lock_interruptible(&npem->npem_lock);
	if (ret)
		return ret;

	if (npem->active_indications & nled->indication->bit)
		val = LED_ON;

	mutex_unlock(&npem->npem_lock);

	return val;
}

int brightness_set(struct led_classdev *led, enum led_brightness brightness)
{
	struct npem_led *nled = container_of(led, struct npem_led, led);
	struct npem *npem = nled->npem;
	u32 indications;
	int ret;

	ret = mutex_lock_interruptible(&npem->npem_lock);
	if (ret)
		return ret;

	if (brightness == LED_OFF)
		indications = npem->active_indications & ~(nled->indication->bit);
	else
		indications = npem->active_indications | nled->indication->bit;

	ret = npem->ops->set_active_indications(npem, indications);

	mutex_unlock(&npem->npem_lock);
	return ret;
}

static void npem_free(struct npem *npem)
{
	struct npem_led *nled;
	int cnt;

	for (cnt = 0; cnt < npem->led_cnt; cnt++) {
		nled = &npem->leds[cnt];

		if (nled->name[0])
			led_classdev_unregister(&nled->led);
	}

	mutex_destroy(&npem->npem_lock);
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
	led->max_brightness = LED_ON;
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
	u32 active;
	int ret;

	npem = kzalloc(struct_size(npem, leds, supported_cnt), GFP_KERNEL);

	if (!npem)
		return -ENOMEM;

	npem->supported_indications = supported;
	npem->led_cnt = supported_cnt;
	npem->pos = pos;
	npem->dev = dev;
	npem->ops = ops;

	ret = ops->get_active_indications(npem, &active);
	if (ret) {
		npem_free(npem);
		return -EACCES;
	}

	npem->active_indications = reg_to_indications(active, ops->inds);

	/*
	 * Do not take npem->npem_lock, get_brightness() is called on
	 * registration path.
	 */
	mutex_init(&npem->npem_lock);

	for_each_indication(indication, npem_indications) {
		if ((npem->supported_indications & indication->bit) == 0)
			/* Do not register unsupported indication */
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
	if (dev->npem)
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
		ret = dsm_get(dev, GET_SUPPORTED_STATES_DSM, &cap);
		if (ret)
			return;

		ops = &dsm_ops;
	}

	ret = pci_npem_init(dev, ops, pos, cap);
	if (ret)
		pci_err(dev, "Failed to register %s PCIe Enclosure Management driver, err: %d\n",
			(ops == &dsm_ops ? "_DSM" : "Native"), ret);
}
