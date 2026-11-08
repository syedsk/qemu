#ifndef HW_PCI_COSIM_BRIDGE_H
#define HW_PCI_COSIM_BRIDGE_H

#include "hw/pci/pci.h"

/*
 * Registry of cosim-forwarding bridges.
 *
 * A cosim bridge's SECONDARY PCIBus carries the cosim_config_* ops
 * (installed in bus->cosim_config_read/write). Buses BELOW the bridge
 * (bus_num > secondary) exist only in the external cosim model, so
 * QEMU has no PCIBus for them. This registry lets core config dispatch
 * locate the forwarding secondary bus by downstream bus number.
 */

/* Register/unregister a cosim-forwarding secondary bus. */
void cosim_bridge_register(PCIBus *sec_bus);
void cosim_bridge_unregister(PCIBus *sec_bus);

/* Return the forwarding secondary bus whose owning bridge's
 * [secondary..subordinate] range contains bus_num, else NULL. */
PCIBus *cosim_bridge_find_bus(uint8_t bus_num);

#endif /* HW_PCI_COSIM_BRIDGE_H */
