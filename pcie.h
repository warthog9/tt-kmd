#ifndef TTDRIVER_PCIE_H_INCLUDED
#define TTDRIVER_PCIE_H_INCLUDED

#include "device.h"

bool safe_pci_restore_state(struct pci_dev *pdev);
bool complete_pcie_init(struct tenstorrent_device *tt_dev, u8 __iomem* reset_unit_regs);
bool pcie_hot_reset_and_restore_state(struct pci_dev *pdev);

#endif
