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
#include <linux/pci_regs.h>
#include <linux/delay.h>

#include "../pci.h"
#include "portdrv.h"

/*
 * Special NPEM & _DSM bits. Not a patterns.
 */
#define	NPEM_ENABLED	BIT(0)
#define	NPEM_RESET	BIT(1)

/* NPEM Command completed */
#define	NPEM_CC		BIT(0)

#define IS_BIT_SET(mask, bit)	((mask & bit) == bit)

static int npem_read_reg(struct npem_device *npem, u16 reg, u32 *val)
{
	int pos = npem->pos + reg;
	int ret = pci_read_config_dword(npem->pdev, pos, val);

	return pcibios_err_to_errno(ret);
}

static int npem_write_ctrl(struct npem_device *npem, u32 reg)
{
	int pos = npem->pos + PCI_NPEM_CTRL;
	int ret =  pci_write_config_dword(npem->pdev, pos, reg);

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
static void wait_for_completion_npem(struct npem_device *npem)
{
	u32 status;
	unsigned long wait_end = jiffies + msecs_to_jiffies(1000);

	while (true) {
		/* Check status only if read is successful. */
		if (npem_read_reg(npem, PCI_NPEM_STATUS, &status) == 0)
			if (IS_BIT_SET(status, NPEM_CC))
				return;

		if (time_after(jiffies, wait_end))
			return;

		usleep_range(10, 15);
	}
}

static int set_active_patterns_npem(struct npem_device *npem, u32 val)
{
	u32 status;
	int ret;

	ret = npem_read_reg(npem, PCI_NPEM_STATUS, &status);
	if (ret != 0)
		return ret;

	if (!IS_BIT_SET(status, NPEM_CC))
		wait_for_completion_npem(npem);

	val = val | NPEM_ENABLED;

	return npem_write_ctrl(npem, val);
}

static int npem_get_active_patterns(struct npem_device *npem, u32 *output)
{
	u32 status;
	int ret;

	ret = npem_read_reg(npem, PCI_NPEM_STATUS, &status);
	if (ret != 0)
		return ret;

	if (IS_BIT_SET(status, NPEM_CC))
		wait_for_completion_npem(npem);

	ret = npem_read_reg(npem, PCI_NPEM_CTRL, output);
	if (ret != 0)
		return ret;

	*output = *output & ~(NPEM_ENABLED | NPEM_RESET);
	return 0;
}

static u32 npem_get_supported_patterns(struct npem_device *npem)
{
	return npem->supported_patterns;
}

static int npem_set_active_patterns(struct npem_device *npem, u32 new_ptrns)
{
	struct npem_device *npem = ecomp->scratch;
	u32 curr_ptrns;

	if (get_active_patterns_npem(npem, &curr_ptrns) != 0)
		return ENCLOSURE_STATUS_CRITICAL;

	if (curr_ptrns == new_ptrns)
		return ENCLOSURE_STATUS_NON_CRITICAL;

	if (set_active_patterns_npem(npem, new_ptrns) != 0)
		return ENCLOSURE_STATUS_CRITICAL;

	return ENCLOSURE_STATUS_OK;
}

static ssize_t active_patterns_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	u32 supported_ptrns, new_ptrns;
	unsigned int val;
	int ret;

	if (kstrtouint(buf, 16, &val) != 0)
		return -EINVAL;

	new_ptrns = (u32) val;

	if(!edev->cb->get_supported_patterns)
		return -EACCES;

	supported_ptrns = npem_get_supported_patterns(edev, ecomp);

	/* Set if requested bits are supported */
	if ((new_ptrns & supported_ptrns) != new_ptrns)
		return -EPERM;

	ret = edev->cb->set_active_patterns(edev, ecomp, new_ptrns);

	if (ret != ENCLOSURE_STATUS_OK) {
		dev_dbg(&edev->edev,
			"Cannot set active_patterns: %s error returned\n",
			enclosure_status[ret]);
		return -EFAULT;
	}
	return count;
}

static ssize_t active_patterns_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct enclosure_device *edev = to_enclosure_device(dev->parent);
	struct enclosure_component *ecomp = to_enclosure_component(dev);
	u32 ptrns = 0;
	enum enclosure_status ret = ENCLOSURE_STATUS_OK;

	if(edev->cb->get_active_patterns)
		ret = edev->cb->get_active_patterns(edev, ecomp, &ptrns);

	if (ret != 0)
		dev_dbg(&edev->edev,
			"Cannot get active_patterns: %d error returned\n", ret);

	return sysfs_emit(buf, "%x\n", ptrns);
}
static DEVICE_ATTR_RW(active_patterns);

static ssize_t supported_patterns_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{

	u32 ptrns = 0;

	if(edev->cb->get_supported_patterns)
		ptrns = edev->cb->get_supported_patterns(edev, ecomp);

	return sysfs_emit(buf, "%x\n", ptrns);
}
static DEVICE_ATTR_RO(supported_patterns);

static struct attribute *npem_attrs[] = {
	&dev_attr_supported_patterns.attr,
	&dev_attr_active_patterns.attr,
	NULL
};
ATTRIBUTE_GROUPS(npem);

void pcie_npem_destroy(struct pci_dev *pdev)
{
	if (pdev->npem)
		kfree(pdev->npem);

	kfree(pdev->npem);
}

void pcie_npem_init(struct pci_dev *pdev)
{
	struct npem_device *npem;
	struct enclosure_device *edev;
	struct enclosure_component *ecomp;
	int pos;
	u32 cap;

	if (pci_pcie_type(pdev) != PCI_EXP_TYPE_DOWNSTREAM)
		return;

	pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_NPEM);
	if (pos == 0)
		return;

	if( pci_read_config_dword(pdev, pos + PCI_NPEM_CAP, &cap) != 0 ||
	    (cap & NPEM_ENABLED) == 0)
		goto err;

	npem = kzalloc(sizeof(*npem), GFP_KERNEL);
	if (!npem)
		goto err;

	npem->supported_patterns = cap & ~(NPEM_ENABLED | NPEM_RESET);
	npem->pdev = pdev;
	pdev->npem = npem;
	return;
err:
	pci_err(pdev, "Failed to register Native PCIe enclosure management driver\n");
}