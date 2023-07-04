// SPDX-License-Identifier: GPL-2.0
/*
 * Native PCIe Enclosure Management
 *	PCie Base Specification r6.0.1-1.0 sec 6.28
 * _DSM Definitions for PCIe SSD Status LED
 *	PCI Firmware Specification Rev 3.3 sec 4.7
 *
 * Copyright (c) 2022 Dell Inc.
 *	Stuart Hayes <stuart.w.hayes@gmail.com>
 * Copyright (c) 2023 Intel Corporation
 *	Mariusz Tkaczyk <mariusz.tkaczyk@linux.intel.com>
 */

#include <linux/acpi.h>
#include <linux/pci.h>
#include <linux/enclosure.h>
#include <linux/delay.h>
#include "pci.h"
#include <linux/pcie_em.h>

/*
 * NPEM & _DSM use the same state bit definitions expect DISABLED.
 * Note: NPEM_RESET in intentionally omitted, it is not a pattern.
 */
#define	NPEM_ENABLED	BIT(0)
#define	NPEM_OK		BIT(2)
#define	NPEM_LOCATE	BIT(3)
#define	NPEM_FAILED	BIT(4)
#define	NPEM_REBUILD	BIT(5)
#define	NPEM_PFA	BIT(6)
#define	NPEM_HOTSPARE	BIT(7)
#define	NPEM_ICA	BIT(8)
#define	NPEM_IFA	BIT(9)
#define	NPEM_IDT	BIT(10)
#define	NPEM_DISABLED	BIT(11)

/* NPEM Command completed */
#define	NPEM_CC		BIT(0)

#define IS_BIT_SET(mask, bit)	((mask & bit) == bit)

static const u32 to_npem[] = {
	[ENCLOSURE_LED_NORMAL]		= NPEM_OK,
	[ENCLOSURE_LED_LOCATE]		= NPEM_LOCATE,
	[ENCLOSURE_LED_FAILURE]		= NPEM_FAILED,
	[ENCLOSURE_LED_REBUILD]		= NPEM_REBUILD,
	[ENCLOSURE_LED_PFA]		= NPEM_PFA,
	[ENCLOSURE_LED_HOTSPARE]	= NPEM_HOTSPARE,
	[ENCLOSURE_LED_ICA]		= NPEM_ICA,
	[ENCLOSURE_LED_IFA]		= NPEM_IFA,
	[ENCLOSURE_LED_IDT]		= NPEM_IDT,
	[ENCLOSURE_LED_DISABLED]	= NPEM_DISABLED,
};

enum pcie_em_type {
	PCIE_EM_DSM,
	PCIE_EM_NPEM,
};

struct private {
	enum pcie_em_type type;
	struct pcie_em_ops *ops;
	u16 npem_pos;
	u32 supported_patterns;
};

/**
 * get_pattern_bit() - get bit value for pattern.
 *
 * Translate enum to bit, handle differences between _DSM and NPEM.
 *
 * Return: 0 on error, bit value otherwise.
 */
static u32 get_pattern_bit(struct private *private,
			   enum enclosure_led_pattern ptrn)
{
	if (ptrn == ENCLOSURE_LED_UNKNOWN)
		return 0;

	if (private->type == PCIE_EM_DSM && ptrn == ENCLOSURE_LED_DISABLED)
		return 0;
	return to_npem[ptrn];
}

struct pcie_em_ops {
	/**
	 * init() - initialize PCIe enclosure management.
	 */
	int (*init)(struct pcie_em_dev *emdev);

	/**
	 * get_active_patterns() - Get active patterns.
	 */
	int (*get_active_patterns)(struct pcie_em_dev *emdev, u32 *output);

	/**
	 * set_active_patterns() - configure active patterns.
	 */
	int (*set_active_patterns)(struct pcie_em_dev *emdev, u32 val);
};

#ifdef CONFIG_ACPI
/*
 * _DSM LED control
 */

struct pcie_em_dsm_output {
	u16 status;
	u8 function_specific_err;
	u8 vendor_specific_err;
	u32 state;
};

static void dsm_status_err_print(struct pci_dev *pdev,
				 struct pcie_em_dsm_output *output)
{
	switch (output->status) {
	case 0:
		break;
	case 1:
		pci_dbg(pdev, "_DSM not supported\n");
		break;
	case 2:
		pci_dbg(pdev, "_DSM invalid input parameters\n");
		break;
	case 3:
		pci_dbg(pdev, "_DSM communication error\n");
		break;
	case 4:
		pci_dbg(pdev, "_DSM function-specific error 0x%x\n",
			output->function_specific_err);
		break;
	case 5:
		pci_dbg(pdev, "_DSM vendor-specific error 0x%x\n",
			output->vendor_specific_err);
		break;
	default:
		pci_dbg(pdev, "_DSM returned unknown status 0x%x\n",
			output->status);
	}
}

#define PCIE_SSD_LEDS_DSM_GUID						\
	GUID_INIT(0x5d524d9d, 0xfff9, 0x4d4b,				\
		  0x8c, 0xb7, 0x74, 0x7e, 0xd5, 0x1e, 0x19, 0x4d)

static const guid_t pcie_pcie_em_dsm_guid = PCIE_SSD_LEDS_DSM_GUID;

#define GET_SUPPORTED_STATES_DSM	0x01
#define GET_STATE_DSM			0x02
#define SET_STATE_DSM			0x03

static int dsm_set(struct pci_dev *pdev, u32 value)
{
	acpi_handle handle;
	union acpi_object *out_obj, arg3[2];
	struct pcie_em_dsm_output *dsm_output;

	handle = ACPI_HANDLE(&pdev->dev);
	if (!handle)
		return -ENODEV;

	arg3[0].type = ACPI_TYPE_PACKAGE;
	arg3[0].package.count = 1;
	arg3[0].package.elements = &arg3[1];

	arg3[1].type = ACPI_TYPE_BUFFER;
	arg3[1].buffer.length = 4;
	arg3[1].buffer.pointer = (u8 *)&value;

	out_obj = acpi_evaluate_dsm_typed(handle, &pcie_pcie_em_dsm_guid,
				1, SET_STATE_DSM, &arg3[0], ACPI_TYPE_BUFFER);
	if (!out_obj)
		return -EIO;

	if (out_obj->buffer.length < 8) {
		ACPI_FREE(out_obj);
		return -EIO;
	}

	dsm_output = (struct pcie_em_dsm_output *)out_obj->buffer.pointer;

	if (dsm_output->status != 0) {
		dsm_status_err_print(pdev, dsm_output);
		ACPI_FREE(out_obj);
		return -EIO;
	}
	ACPI_FREE(out_obj);
	return 0;
}

static int dsm_get(struct pci_dev *pdev, u64 dsm_func, u32 *output)
{
	acpi_handle handle;
	union acpi_object *out_obj;
	struct pcie_em_dsm_output *dsm_output;

	handle = ACPI_HANDLE(&pdev->dev);
	if (!handle)
		return -ENODEV;

	out_obj = acpi_evaluate_dsm_typed(handle, &pcie_pcie_em_dsm_guid, 0x1,
					  dsm_func, NULL, ACPI_TYPE_BUFFER);
	if (!out_obj)
		return -EIO;

	if (out_obj->buffer.length < 8) {
		ACPI_FREE(out_obj);
		return -EIO;
	}

	dsm_output = (struct pcie_em_dsm_output *)out_obj->buffer.pointer;
	if (dsm_output->status != 0) {
		dsm_status_err_print(pdev, dsm_output);
		ACPI_FREE(out_obj);
		return -EIO;
	}

	*output = dsm_output->state;
	ACPI_FREE(out_obj);
	return 0;
}

static int init_dsm(struct pcie_em_dev *emdev)
{
	struct pci_dev *pdev = emdev->pdev;
	struct private *private = emdev->private;

	if (dsm_get(pdev, GET_SUPPORTED_STATES_DSM,
		    &private->supported_patterns) != 0)
		return -EPERM;

	if (IS_BIT_SET(private->supported_patterns, NPEM_ENABLED))
		return -EPERM;

	return 0;
}

static int get_active_patterns_dsm(struct pcie_em_dev *emdev, u32 *output)
{
	return dsm_get(emdev->pdev, GET_STATE_DSM, output);
}

static int set_active_patterns_dsm(struct pcie_em_dev *emdev, u32 output)
{
	return dsm_set(emdev->pdev, output);
}

static struct pcie_em_ops dsm_ops = {
	.init		= init_dsm,
	.get_active_patterns	= get_active_patterns_dsm,
	.set_active_patterns	= set_active_patterns_dsm,
};

static bool pcie_has_dsm(struct pci_dev *pdev)
{
	acpi_handle handle;
	const guid_t pcie_ssd_leds_dsm_guid = PCIE_SSD_LEDS_DSM_GUID;

	handle = ACPI_HANDLE(&pdev->dev);
	if (!handle)
		return false;

	if (acpi_check_dsm(handle, &pcie_ssd_leds_dsm_guid, 0x1,
			   1 << GET_SUPPORTED_STATES_DSM ||
			   1 << GET_STATE_DSM || 1 << SET_STATE_DSM) == true)
		return true;
	return false;
}
#endif /* CONFIG_ACPI */

/*
 * NPEM LED control
 */
static bool pci_has_npem(struct pci_dev *pdev)
{
	int pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_NPEM);
	u32 cap;

	if (!pos)
		return false;

	if (pci_read_config_dword(pdev, pos + PCI_NPEM_CAP, &cap) == 0)
		if (IS_BIT_SET(cap, NPEM_ENABLED))
			return true;
	return false;
}

static int npem_read_reg(struct pcie_em_dev *emdev, u16 reg, u32 *val)
{
	struct private *private = emdev->private;
	int pos = private->npem_pos + reg;
	int ret = pci_read_config_dword(emdev->pdev, pos, val);

	return pcibios_err_to_errno(ret);
}

static int npem_write_ctrl(struct pcie_em_dev *emdev, u32 reg)
{
	struct private *private = emdev->private;
	int pos = private->npem_pos + PCI_NPEM_CTRL;
	int ret =  pci_write_config_dword(emdev->pdev, pos, reg);

	return pcibios_err_to_errno(ret);
}

/**
 * wait_for_completion_npem() - wait to command completed status bit to be high.
 *
 * If the bit is not set within 1 second limit on command execution, software
 * is permitted to repeat the NPEM command or issue the next NPEM command.
 *
 * For the case where an NPEM command has not completed when software polls the
 * bit, it is recommended that software not continuously “spin” on polling the
 * bit, but rather poll under interrupt at a reduced rate; for example at 10 ms
 * intervals.
 */
static void wait_for_completion_npem(struct pcie_em_dev *emdev)
{
	u32 status;
	unsigned long wait_end = jiffies + msecs_to_jiffies(1000);

	while (true) {
		/* Check status only if read is successful. */
		if (npem_read_reg(emdev, PCI_NPEM_STATUS, &status) == 0)
			if (IS_BIT_SET(status, NPEM_CC))
				return;

		if (time_after(jiffies, wait_end))
			return;

		usleep_range(10, 15);
	}
}

static int set_active_patterns_npem(struct pcie_em_dev *emdev, u32 val)
{
	u32 status;
	int ret;

	ret = npem_read_reg(emdev, PCI_NPEM_STATUS, &status);
	if (ret != 0)
		return ret;

	if (!IS_BIT_SET(status, NPEM_CC))
		wait_for_completion_npem(emdev);

	val = val | NPEM_ENABLED;

	return npem_write_ctrl(emdev, val);
}

static int get_active_patterns_npem(struct pcie_em_dev *emdev, u32 *output)
{
	u32 status;
	int ret;

	ret = npem_read_reg(emdev, PCI_NPEM_STATUS, &status);
	if (ret != 0)
		return ret;

	if (IS_BIT_SET(status, NPEM_CC))
		wait_for_completion_npem(emdev);

	ret = npem_read_reg(emdev, PCI_NPEM_CTRL, output);
	if (ret != 0)
		return ret;

	return 0;
}

/**
 * init_npem() - initialize Native PCIe enclosure management.
 * @emdev: pcie_em device.
 *
 * Check if NPEM capability exists, load supported NPEM capabilities and
 * determine if NPEM is enabled.
 */
static int init_npem(struct pcie_em_dev *emdev)
{
	struct pci_dev *pdev = emdev->pdev;
	struct private *private = emdev->private;
	int ret;

	private->npem_pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_NPEM);
	if (private->npem_pos == 0)
		return -EFAULT;

	ret = npem_read_reg(emdev, PCI_NPEM_CAP, &private->supported_patterns);
	if (ret != 0)
		return ret;

	if (!IS_BIT_SET(private->supported_patterns, NPEM_ENABLED))
		return -EPERM;

	return 0;
}

static struct pcie_em_ops npem_ops = {
	.init			= init_npem,
	.get_active_patterns	= get_active_patterns_npem,
	.set_active_patterns	= set_active_patterns_npem,
};

/*
 * end of NPEM code
 */

static bool pcie_em_is_pattern_supported(struct enclosure_device *edev,
					 struct enclosure_component *ecomp,
					 enum enclosure_led_pattern ptrn)
{
	struct pcie_em_dev *emdev = ecomp->scratch;
	struct private *private = emdev->private;
	u32 new_ptrn = get_pattern_bit(private, ptrn);

	if (new_ptrn == 0)
		return false;

	if (IS_BIT_SET(private->supported_patterns, new_ptrn))
		return true;
	return false;
}

static bool pcie_em_get_pattern_state(struct enclosure_device *edev,
				      struct enclosure_component *ecomp,
				      enum enclosure_led_pattern ptrn)
{
	struct pcie_em_dev *emdev = ecomp->scratch;
	struct private *private = emdev->private;
	u32 new_ptrn = get_pattern_bit(private, ptrn);
	u32 curr_ptrns;

	if (new_ptrn == 0)
		return ENCLOSURE_STATUS_UNSUPPORTED;

	if (private->ops->get_active_patterns(emdev, &curr_ptrns) != 0)
		return false;

	if (IS_BIT_SET(curr_ptrns, new_ptrn))
		return true;

	return false;
}

static enum enclosure_status
pcie_em_set_pattern_state(struct enclosure_device *edev,
			  struct enclosure_component *ecomp,
			  enum enclosure_led_pattern ptrn,
			  bool state)
{
	struct pcie_em_dev *emdev = ecomp->scratch;
	struct private *private = emdev->private;
	u32 new_ptrn = get_pattern_bit(private, ptrn);
	u32 curr_ptrns, new_ptrns;

	if (new_ptrn == 0)
		return ENCLOSURE_STATUS_UNSUPPORTED;

	if (private->ops->get_active_patterns(emdev, &curr_ptrns) != 0)
		return ENCLOSURE_STATUS_CRITICAL;

	if (state == IS_BIT_SET(curr_ptrns, new_ptrn))
		return ENCLOSURE_STATUS_NON_CRITICAL;


	if (state == true)
		new_ptrns = (curr_ptrns | new_ptrn);
	else
		new_ptrns = (curr_ptrns & ~new_ptrn);

	if (private->ops->set_active_patterns(emdev, new_ptrns) != 0)
		return ENCLOSURE_STATUS_CRITICAL;

	return ENCLOSURE_STATUS_OK;
}

static struct enclosure_component_callbacks pcie_em_cb = {
	.is_pattern_supported	= pcie_em_is_pattern_supported,
	.get_pattern_state	= pcie_em_get_pattern_state,
	.set_pattern_state	= pcie_em_set_pattern_state,
};

struct private *get_private(enum pcie_em_type type)
{
	struct private *private = kzalloc(sizeof(*private), GFP_KERNEL);

	if (!private)
		return NULL;

#ifdef CONFIG_ACPI
	if (type == PCIE_EM_DSM)
		private->ops = &dsm_ops;
#endif /* CONFIG_ACPI */

	if (type == PCIE_EM_NPEM)
		private->ops = &npem_ops;

	if (!private->ops)
		return NULL;

	private->type = type;
	return private;
}

void pcie_em_release_dev(struct pcie_em_dev *emdev)
{
	if (emdev->edev) {
		emdev->edev->scratch = NULL;
		enclosure_unregister(emdev->edev);
	}

	kfree(emdev->private);
	kfree(emdev);
}

static struct pcie_em_dev *pcie_em_create_dev(struct pci_dev *pdev,
					      enum pcie_em_type type)
{
	struct pcie_em_dev *emdev;
	struct enclosure_device *edev;
	struct enclosure_component *ecomp;
	struct private *private;
	int rc = 0;

	pci_info(pdev, "Registering PCIe Enclosure management\n");

	emdev = kzalloc(sizeof(*emdev), GFP_KERNEL);
	if (!emdev)
		goto err;

	emdev->pdev = pdev;

	private = get_private(type);
	if (private == NULL)
		goto err;
	emdev->private = private;

	if (private->ops->init(emdev))
		goto err;

	edev = enclosure_register(&pdev->dev, dev_name(&pdev->dev), 1,
				  &pcie_em_cb);
	if (!edev)
		goto err;

	ecomp = enclosure_component_alloc(edev, 0, ENCLOSURE_COMPONENT_DEVICE,
					  dev_name(&pdev->dev));
	if (IS_ERR(ecomp))
		goto err;

	ecomp->type = ENCLOSURE_COMPONENT_ARRAY_DEVICE;
	rc = enclosure_component_register(ecomp);
	if (rc < 0)
		goto err;

	ecomp->scratch = emdev;
	emdev->edev = edev;
	return emdev;
err:
	pci_err(pdev, "Failed to register PCIe enclosure management driver\n");
	if (emdev)
		pcie_em_release_dev(emdev);
	return NULL;
}

struct pcie_em_dev *get_pcie_enclosure_management(struct pci_dev *pdev)
{
#ifdef CONFIG_ACPI
	if (pcie_has_dsm(pdev))
		return pcie_em_create_dev(pdev, PCIE_EM_DSM);
#endif /* CONFIG_ACPI */
	if (pci_has_npem(pdev))
		return pcie_em_create_dev(pdev, PCIE_EM_NPEM);
	return NULL;
}
