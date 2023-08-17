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
 * Special NPEM & _DSM bits. Not a patterns.
 */
#define	NPEM_ENABLED	BIT(0)
#define	NPEM_RESET	BIT(1)

/* NPEM Command completed */
#define	NPEM_CC		BIT(0)

#define IS_BIT_SET(mask, bit)	((mask & bit) == bit)

struct private {
	struct pcie_em_ops *ops;
	u16 npem_pos;
	u32 supported_patterns;
};

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

	*output = *output & ~(NPEM_ENABLED | NPEM_RESET);
	return 0;
}

/**
 * init_npem() - initialize Native PCIe enclosure management.
 * @emdev: pcie_em device.
 *
 * Check if NPEM capability exists, load supported NPEM capabilities and
 * determine if NPEM is enabled. Return error if not.
 */
static int init_npem(struct pcie_em_dev *emdev)
{
	struct pci_dev *pdev = emdev->pdev;
	struct private *private = emdev->private;
	u32 ptrns;
	int ret;

	private->npem_pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_NPEM);
	if (private->npem_pos == 0)
		return -EFAULT;

	ret = npem_read_reg(emdev, PCI_NPEM_CAP, &ptrns);
	if (ret != 0)
		return ret;

	/* Don't register device if NPEM is not enabled */
	if (!(ptrns & NPEM_ENABLED))
		return -EPERM;

	private->supported_patterns = ptrns & ~(NPEM_ENABLED | NPEM_RESET);
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

static u32 pcie_em_get_supported_patterns(struct enclosure_device *edev,
					   struct enclosure_component *ecomp)
{
	struct pcie_em_dev *emdev = ecomp->scratch;
	struct private *private = emdev->private;

	return private->supported_patterns;
}

static int pcie_em_get_active_patterns(struct enclosure_device *edev,
				       struct enclosure_component *ecomp,
				       u32 *ptrns)
{
	struct pcie_em_dev *emdev = ecomp->scratch;
	struct private *private = emdev->private;

	return private->ops->get_active_patterns(emdev, ptrns);
}
static enum enclosure_status
pcie_em_set_active_patterns(struct enclosure_device *edev,
		     struct enclosure_component *ecomp, u32 new_ptrns)
{
	struct pcie_em_dev *emdev = ecomp->scratch;
	struct private *private = emdev->private;
	u32 curr_ptrns;

	if (private->ops->get_active_patterns(emdev, &curr_ptrns) != 0)
		return ENCLOSURE_STATUS_CRITICAL;

	if (curr_ptrns == new_ptrns)
		return ENCLOSURE_STATUS_NON_CRITICAL;

	if (private->ops->set_active_patterns(emdev, new_ptrns) != 0)
		return ENCLOSURE_STATUS_CRITICAL;

	return ENCLOSURE_STATUS_OK;
}

static struct enclosure_component_callbacks pcie_em_cb = {
	.get_supported_patterns	= pcie_em_get_supported_patterns,
	.get_active_patterns	= pcie_em_get_active_patterns,
	.set_active_patterns	= pcie_em_set_active_patterns,
};


void pcie_em_release_dev(struct pcie_em_dev *emdev)
{
	if (emdev->edev) {
		emdev->edev->scratch = NULL;
		enclosure_unregister(emdev->edev);
	}

	kfree(emdev->private);
	kfree(emdev);
}

static struct pcie_em_dev *pcie_em_create_dev(struct pci_dev *pdev)
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

	private = kzalloc(sizeof(*private), GFP_KERNEL);
	if (!private)
		goto err;

	private->ops = &npem_ops;
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
	if (pci_has_npem(pdev))
		return pcie_em_create_dev(pdev);
	return NULL;
}
