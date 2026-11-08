#include "qemu/osdep.h"
#include "qemu/queue.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/cosim_bridge.h"

/*
 * Pure list of cosim-forwarding secondary buses. The owning bridge
 * PCIDevice is derived from sec_bus->parent_dev, so we only store the
 * bus pointer.
 */
typedef struct CosimBridgeEntry {
    PCIBus *sec_bus;
    QLIST_ENTRY(CosimBridgeEntry) node;
} CosimBridgeEntry;

static QLIST_HEAD(, CosimBridgeEntry) cosim_bridges =
    QLIST_HEAD_INITIALIZER(cosim_bridges);

void cosim_bridge_register(PCIBus *sec_bus)
{
    CosimBridgeEntry *e = g_new0(CosimBridgeEntry, 1);
    e->sec_bus = sec_bus;
    QLIST_INSERT_HEAD(&cosim_bridges, e, node);
}

void cosim_bridge_unregister(PCIBus *sec_bus)
{
    CosimBridgeEntry *e, *tmp;
    QLIST_FOREACH_SAFE(e, &cosim_bridges, node, tmp) {
        if (e->sec_bus == sec_bus) {
            QLIST_REMOVE(e, node);
            g_free(e);
            return;
        }
    }
}

PCIBus *cosim_bridge_find_bus(uint8_t bus_num)
{
    CosimBridgeEntry *e;
    QLIST_FOREACH(e, &cosim_bridges, node) {
        /* bridge owning this secondary bus */
        PCIDevice *d = e->sec_bus->parent_dev;
        if (!d) {
            continue;
        }
        uint8_t sec = d->config[PCI_SECONDARY_BUS];
        uint8_t sub = d->config[PCI_SUBORDINATE_BUS];
		
        if (sec != 0 && sec <= bus_num && bus_num <= sub) {
            return e->sec_bus;
        }
    }
    return NULL;
}

