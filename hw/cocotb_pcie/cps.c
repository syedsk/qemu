#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/pci/pci.h"
#include "hw/pci/pcie.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/cosim_bridge.h"
#include "hw/qdev-properties.h"
#include "qemu/log.h"
#include "cosim_port.h"
#include "cps.h"

#define TYPE_COCOTB_PCIE_SWITCH "cocotb-pcie-switch"
#define CPS(obj) OBJECT_CHECK(CpsCtrl, (obj), TYPE_COCOTB_PCIE_SWITCH)

/* --- secondary-bus (and below) config -> cosim as Type 1 --------- */
static uint32_t cps_sec_config_read(PCIBus *bus, uint8_t bus_num, int devfn,
                                    uint32_t addr, int len, void *opaque)
{
    CpsCtrl *cps = opaque;
    /* use the target bus number, not the sec bus: buses below the
     * switch DSPs have no QEMU PCIBus and are reached via this one */
    uint16_t bdf = ((uint16_t)bus_num << 8) | devfn;
    return cosim_config_read(&cps->port, bdf, addr, len, /*type1=*/true);
}

static void cps_sec_config_write(PCIBus *bus, uint8_t bus_num, int devfn,
                                 uint32_t addr, uint32_t val,
                                 int len, void *opaque)
{
    CpsCtrl *cps = opaque;
    uint16_t bdf = ((uint16_t)bus_num << 8) | devfn;
    cosim_config_write(&cps->port, bdf, addr, val, len, /*type1=*/true);
}

/* --- bridge's OWN header -> cosim as Type 0 ---------------------- */
static uint32_t cps_config_read(PCIDevice *dev, uint32_t addr, int len)
{
    CpsCtrl *cps = CPS(dev);
    return cosim_config_read(&cps->port, pci_get_bdf(dev),
                             addr, len, /*type1=*/false);
}

static void cps_config_write(PCIDevice *dev, uint32_t addr,
                             uint32_t val, int len)
{
    CpsCtrl *cps = CPS(dev);

#ifdef DEBUG
    fprintf(stderr, "cps_config_write: addr: %u val:%u\n ", addr, val);
#endif

    /* forward to cocotb (SwitchUpstreamPort owns the real header) */
    cosim_config_write(&cps->port, pci_get_bdf(dev),
                       addr, val, len, /*type1=*/false);

    /* run bridge default so QEMU tracks sec/sub bus numbers and
     * mem base/limit windows (needed for routing + MMIO gating) */
    pci_bridge_write_config(dev, addr, val, len);
}

/* --- MMIO passthrough window behind the bridge ------------------- */
static uint64_t cps_win_read(void *opaque, hwaddr addr, unsigned size)
{
    CpsCtrl   *cps = (CpsCtrl *)opaque;
    return cosim_mmio_read(&cps->port, addr, size);
}
static void cps_win_write(void *opaque, hwaddr addr,
                          uint64_t data, unsigned size)
{
    CpsCtrl   *cps = (CpsCtrl *)opaque;
    cosim_mmio_write(&cps->port, addr, data, size);
}
static const MemoryRegionOps cps_win_ops = {
    .read = cps_win_read,
    .write = cps_win_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl  = { .min_access_size = 1, .max_access_size = 8 },
};

static void cps_realize(PCIDevice *dev, Error **errp)
{
    CpsCtrl   *cps = CPS(dev);
    PCIBridge *br  = PCI_BRIDGE(dev);
    PCIBus    *sec;
    int rc;

    cps->port.np_port   = cps->np_port;
    cps->port.push_port = cps->push_port;
    cps->port.pull_port = cps->pull_port;
    cps->port.next_tag  = 0;
    cps->port.dma_dev   = dev;

    if (cosim_port_connect(&cps->port, errp) < 0) {
        return;
    }

    rc = pcie_cap_init(dev, 0x80, PCI_EXP_TYPE_UPSTREAM, 0, errp);
    if (rc < 0) {
        return;
    }

    pci_bridge_initfn(dev, TYPE_PCIE_BUS);
    sec = pci_bridge_get_sec_bus(br);
    cps->sec_bus = sec;

    /* install cosim config forwarders on the secondary bus */
    sec->cosim_config_read  = cps_sec_config_read;
    sec->cosim_config_write = cps_sec_config_write;
    sec->cosim_opaque       = cps;

    /* register for bus-range lookup from core hooks */
    cosim_bridge_register(sec);

    /*
     * MMIO passthrough: cover the whole secondary-bus memory space.
     * pci_bridge_update_mappings() aliases the guest-programmed
     * mem / prefetchable base..limit windows of this bridge from the
     * parent bus into br->address_space_mem at the SAME bus address,
     * so 'addr' in cps_win_read/write is the absolute PCI bus address
     * and gating by base/limit + Command.MEM is done by QEMU.
     */
    memory_region_init_io(&cps->mem_window, OBJECT(cps), &cps_win_ops,
                          cps, "cps-mem-window", UINT64_MAX);
    memory_region_add_subregion_overlap(&br->address_space_mem, 0,
                                        &cps->mem_window, -1);

    qemu_log("%s: cocotb switch proxy ready (sec bus created)\n", __func__);
}

static void cps_exit(PCIDevice *dev)
{
    CpsCtrl *cps = CPS(dev);
    cosim_bridge_unregister(cps->sec_bus);
    memory_region_del_subregion(&PCI_BRIDGE(dev)->address_space_mem,
                                &cps->mem_window);
    cosim_port_disconnect(&cps->port);
    pci_bridge_exitfn(dev);
}

static Property cps_properties[] = {
    DEFINE_PROP_UINT16("np_port",   CpsCtrl, np_port,   5555),
    DEFINE_PROP_UINT16("push_port", CpsCtrl, push_port, 6000),
    DEFINE_PROP_UINT16("pull_port", CpsCtrl, pull_port, 6001),
    DEFINE_PROP_END_OF_LIST(),
};

static void cps_class_init(ObjectClass *oc, void *data)
{
    DeviceClass    *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);

    pc->realize      = cps_realize;
    pc->exit         = cps_exit;
    pc->config_read  = cps_config_read;
    pc->config_write = cps_config_write;
    pc->vendor_id    = 0x1234;
    pc->device_id    = 0x0003;
    pc->class_id     = PCI_CLASS_BRIDGE_PCI;

    device_class_set_props(dc, cps_properties);
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
}

static const TypeInfo cps_info = {
    .name          = TYPE_COCOTB_PCIE_SWITCH,
    .parent        = TYPE_PCI_BRIDGE,
    .instance_size = sizeof(CpsCtrl),
    .class_init    = cps_class_init,
    .interfaces    = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { },
    },
};

static void cps_register(void) { type_register_static(&cps_info); }
type_init(cps_register);

