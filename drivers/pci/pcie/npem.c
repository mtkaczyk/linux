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
#include <linux/errno.h>
#include <linux/kstrtox.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/sysfs.h>
#include <linux/types.h>

#include "../pci.h"

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
	struct mutex * npem_lock;
	u16 pos;
	u32 supported_patterns;
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

static int npem_read_cc_status(struct npem_device *npem)
{
	int val = 0;
	int ret = npem_read_reg(npem, PCI_NPEM_STATUS, &val);

	if (ret)
		return 0;
	return val;
}

static int npem_set_active_patterns(struct npem_device *npem, u32 val)
{
	int cc_status;
	int ret;

	lockdep_assert_held(npem->npem_lock);

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
	return read_poll_timeout(npem_read_cc_status, cc_status, cc_status, 15,
			  1000000, false, npem);
}

static int npem_get_active_patterns(struct npem_device *npem, u32 *output)
{
	int ret;

	lockdep_assert_held(npem->npem_lock);

	ret = npem_read_reg(npem, PCI_NPEM_CTRL, output);
	if (ret != 0)
		return ret;

	*output &= ~(NPEM_ENABLED | NPEM_RESET);
	mutex_unlock(npem->npem_lock);
	return 0;
}

static ssize_t active_patterns_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct npem_device *npem = pdev->npem;
	u32 new_ptrns, curr_ptrns;
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 16, &val);
	if (ret)
		return ret;

	new_ptrns = val;

	/* Set if requested bits are supported */
	if ((new_ptrns & npem->supported_patterns) != new_ptrns)
		return -EPERM;

	ret = mutex_lock_interruptible(npem->npem_lock);
	if (ret)
		return ret;

	ret = npem_get_active_patterns(npem, &curr_ptrns);
	if (ret)
		goto out_unlock;

	if (new_ptrns != curr_ptrns)
		ret = npem_set_active_patterns(npem, new_ptrns);
	else
		ret = -EPERM;

out_unlock:
	mutex_unlock(npem->npem_lock);
	return ret ? ret : count;
}

static ssize_t active_patterns_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct npem_device *npem = pdev->npem;
	u32 ptrns;

	int ret = mutex_lock_interruptible(npem->npem_lock);
	if (ret)
		goto out;

	ret = npem_get_active_patterns(npem, &ptrns);

	if (ret)
		ptrns = 0;

	mutex_unlock(npem->npem_lock);
out:
	return sysfs_emit(buf, "%x8\n", ptrns);
}
static DEVICE_ATTR_RW(active_patterns);

static ssize_t supported_patterns_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct npem_device *npem = pdev->npem;

	return sysfs_emit(buf, "%x8\n", npem->supported_patterns);
}
static DEVICE_ATTR_RO(supported_patterns);

static struct attribute *npem_stats_attrs[] __ro_after_init = {
	&dev_attr_active_patterns.attr,
	&dev_attr_supported_patterns.attr,
	NULL
};

static umode_t
npem_is_visible(struct kobject *kobj, struct attribute *attr, int i)
{
	struct device *dev = kobj_to_dev(kobj);
	struct pci_dev *pdev = to_pci_dev(dev);

	if (!pdev->npem)
		return 0;

	return attr->mode;
}

const struct attribute_group npem_attr_group = {
	.attrs  = npem_stats_attrs,
	.is_visible = npem_is_visible,
};

void pcie_npem_destroy(struct pci_dev *dev)
{
	kfree(dev->npem);
}

void pcie_npem_init(struct pci_dev *dev)
{
	struct npem_device *npem;
	int pos;
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

	npem->pos = pos;
	npem->supported_patterns = cap & ~(NPEM_ENABLED | NPEM_RESET);
	npem->dev = dev;
	dev->npem = npem;
	mutex_init(npem->npem_lock);
}
