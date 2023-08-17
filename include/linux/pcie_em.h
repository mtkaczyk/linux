/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Native PCIe Enclosure Management
 *	PCie Base Specification r6.0.1-1.0 sec 6.28
 *
 * Copyright (c) 2022 Intel Corporation
 *	Mariusz Tkaczyk <mariusz.tkaczyk@linux.intel.com>
 */

#ifndef DRIVERS_PCI_EM_H
#define DRIVERS_PCI_EM_H

/**
 * struct pcie_em_dev - PCIe Enclosure Management device.
 * @pdev: PCI device.
 * @edev: enclosure device.
 * @private: Internal properties and callbacks.
 */
struct pcie_em_dev {
	struct pci_dev *pdev;
	struct enclosure_device *edev;

	void *private;
};

struct pcie_em_dev *get_pcie_enclosure_management(struct pci_dev *pdev);
void pcie_em_release_dev(struct pcie_em_dev *emdev);

#endif /* DRIVERS_PCI_PCIE_EM_H */
