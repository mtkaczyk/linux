// SPDX-License-Identifier: GPL-2.0
/*
 * Native PCIe Enclosure Management
 *	PCie Base Specification r6.0.1-1.0 sec 6.28
 * _DSM Definitions for PCIe SSD Status LED
 *	PCI Firmware Specification Rev 3.3 sec 4.7
 *
 * Copyright (c) 2022 Dell Inc.
 *	Stuart Hayes <stuart.w.hayes@gmail.com>
 * Copyright (c) 2022 Intel Corporation
 *	Mariusz Tkaczyk <mariusz.tkaczyk@linux.intel.com>
 */

#ifndef DRIVERS_PCI_EM_H
#define DRIVERS_PCI_EM_H

enum pcie_em_type {
	PCIE_EM_NOT_SUPPORTED = 0,
	PCIE_EM_DSM,
	PCIE_EM_NPEM,
};

/**
 * struct pcie_em_dev - PCIe Enclosure Management device.
 * @pdev: PCI device.
 * @edev: enclosure device.
 * @supported_patterns: list of supported states.
 * @npem_pos: position of NPEM ext cap.
 */
struct pcie_em_dev {
	struct pci_dev *pdev;
	struct enclosure_device *edev;

	u32 supported_patterns;
	void *private;
};

enum pcie_em_type get_pcie_enclosure_management(struct pci_dev *pdev);
struct pcie_em_dev *pcie_em_create_dev(struct pci_dev *pdev,
				       enum pcie_em_type type);
void pcie_em_release_dev(struct pcie_em_dev *emdev);

#endif /* DRIVERS_PCI_PCIE_EM_H */
