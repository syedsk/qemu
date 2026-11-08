#ifndef COCOTB_PCIE_SWITCH_H
#define COCOTB_PCIE_SWITCH_H

#include "hw/pci/pci_device.h"
#include "hw/pci/pci_bridge.h"
#include "exec/memory.h"
#include "qapi/error.h"
#include "cpd.h"

typedef struct cocotb_pcie_switch_s {
    PCIBridge    parent_obj;

    CosimPort    port;          /* cosim transport context */
    PCIBus      *sec_bus;

    MemoryRegion mem_window;

    uint16_t     np_port;
    uint16_t     push_port;
    uint16_t     pull_port;
} CpsCtrl;

#endif

