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
#include <linux/enclosure.h>
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

struct npem_device {
	struct pci_dev *pdev;
	struct enclosure_device *edev;

	u16 pos;
	u32 supported_patterns;
};

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

static int get_active_patterns_npem(struct npem_device *npem, u32 *output)
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

/**
 * init_npem() - initialize Native PCIe enclosure management.
 * @emdev: pcie_em device.
 *
 * Check if NPEM capability exists, load supported NPEM capabilities and
 * determine if NPEM is enabled. Return error if not.
 */
static int init_npem(struct npem_device *npem)
{
	struct pci_dev *pdev = npem->pdev;
	u32 ptrns;
	int ret;

	npem->pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_NPEM);
	if (npem->pos == 0)
		return -EFAULT;

	ret = npem_read_reg(npem, PCI_NPEM_CAP, &ptrns);
	if (ret != 0)
		return ret;

	/* Don't register device if NPEM is not enabled */
	if (!(ptrns & NPEM_ENABLED))
		return -EPERM;

	npem->supported_patterns = ptrns & ~(NPEM_ENABLED | NPEM_RESET);
	return 0;
}

static u32 npem_get_supported_patterns(struct enclosure_device *edev,
				       struct enclosure_component *ecomp)
{
	struct npem_device *npem = ecomp->scratch;

	return npem->supported_patterns;
}

static int npem_get_active_patterns(struct enclosure_device *edev,
				    struct enclosure_component *ecomp,
				    u32 *ptrns)
{
	struct npem_device *npem = ecomp->scratch;

	return get_active_patterns_npem(npem, ptrns);
}
static enum enclosure_status
npem_set_active_patterns(struct enclosure_device *edev,
			 struct enclosure_component *ecomp, u32 new_ptrns)
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

static struct enclosure_component_callbacks pcie_em_cb = {
	.get_supported_patterns	= npem_get_supported_patterns,
	.get_active_patterns	= npem_get_active_patterns,
	.set_active_patterns	= npem_set_active_patterns,
};

static struct npem_device *npem_create_dev(struct pci_dev *pdev)
{
	struct npem_device *npem;
	struct enclosure_device *edev;
	struct enclosure_component *ecomp;
	int rc = 0;

	npem = kzalloc(sizeof(*npem), GFP_KERNEL);
	if (!npem)
		goto err;

	npem->pdev = pdev;

	if (init_npem(npem))
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

	ecomp->scratch = npem;
	npem->edev = edev;
	return npem;
err:
	pci_err(pdev, "Failed to register PCIe enclosure management driver\n");
	return NULL;
}

int npem_probe(struct pcie_device *dev)
{
	struct pci_dev *pdev = dev->port;
	int pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_NPEM);
	u32 cap;
	int ret;

	if (!pos)
		return -ENXIO;

	ret = pci_read_config_dword(pdev, pos + PCI_NPEM_CAP, &cap);
	if (ret != 0)
		return ret;

	if ((cap & NPEM_ENABLED) == 0)
		return -EPERM;

	if (!npem_create_dev(pdev))
		return -ENXIO;

	return 0;
}

void npem_remove(struct pcie_device *dev)
{
	return;
	//struct pci_dev *pdev = dev->pdev;

	//if (emdev->edev) {
	//	emdev->edev->scratch = NULL;
	//	enclosure_unregister(emdev->edev);
	//}

	//kfree(emdev->private);
	//kfree(emdev);
}

static struct pcie_port_service_driver npemdriver = {
	.name		= "npem",
	.port_type	= PCIE_ANY_PORT,
	.service	= PCIE_PORT_SERVICE_NPEM,
	.probe		= npem_probe,
	.remove		= npem_remove,
};

int __init pcie_npem_init(void)
{
	return pcie_port_service_register(&npemdriver);
}