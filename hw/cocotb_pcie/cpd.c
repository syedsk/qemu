/*
 * cpd.c
 *
 * @author Syed Syed
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */
#include "qemu/osdep.h"

#include "qapi/error.h"
#include "hw/pci/pci.h"
#include "hw/pci/pcie.h"
#include "hw/qdev-properties.h"
#include "qemu/units.h"
#include "cpd.h"
#include "qemu/log.h"

/* Implementation of Cocotb pcie device (cpd)
 * which talks to external simulation of PCIe endpoint on cocotb
 * Initially tested against nvme endpoint. But can talk to any flaveor of pci endpoint
 * NIC, GPU etc
 */


#define TYPE_COCOTB_PCIE "cocotb-pcie-endpoint"
#define CPD(obj)    OBJECT_CHECK(CpdCtrl, (obj), TYPE_COCOTB_PCIE)


static uint64_t cpd_mmio_0_read(void *opaque, hwaddr offset, unsigned size)
{
    CpdCtrl *cpd = CPD(opaque);
    MemoryRegion *mr;

    if (!cpd) {
        qemu_log("%s: improper rd access to %lu\n", __func__, offset);
        return 0;
    }

    mr = &cpd->bar0;

	/* since we are sending tlp we need to put full addr on pcie bus
	 */
	hwaddr addr = mr->addr + offset;

	return cosim_mmio_read(cpd, addr, size);
}

static void cpd_mmio_0_write(void *opaque, hwaddr offset, uint64_t data, unsigned size)
{
    CpdCtrl *cpd = CPD(opaque);
    MemoryRegion *mr;

    if (!cpd) {
        qemu_log("%s: improper wr access to %lu\n", __func__, offset);
        return;
    }

    mr = &cpd->bar0;

	/* since we are sending tlp we need to put full addr on pcie bus
	 */
	hwaddr addr = mr->addr + offset;
	#ifdef DEBUG
    printf("%s addr: %llx data: %lx\n", __func__, (unsigned long long)addr, data);
	#endif

	return cosim_mmio_write(cpd, addr, data, size);
}

static uint64_t cpd_mmio_1_read(void *opaque, hwaddr offset, unsigned size)
{
    CpdCtrl *cpd = CPD(opaque);
    MemoryRegion *mr;

    if (!cpd) {
        qemu_log("%s: improper rd access to %lu\n", __func__, offset);
        return 0;
    }

    mr = &cpd->msix_exclusive_bar;

	/* since we are sending tlp we need to put full addr on pcie bus
	 */
	hwaddr addr = mr->addr + offset;

	return cosim_mmio_read(cpd, addr, size);
}

static void cpd_mmio_1_write(void *opaque, hwaddr offset, uint64_t data, unsigned size)
{
    CpdCtrl *cpd = CPD(opaque);
    MemoryRegion *mr;

    if (!cpd) {
        qemu_log("%s: improper wr access to %lu\n", __func__, offset);
        return;
    }

    mr = &cpd->msix_exclusive_bar;

	/* since we are sending tlp we need to put full addr on pcie bus
	 */
	hwaddr addr = mr->addr + offset;
	#ifdef DEBUG
    printf("%s addr: %llx data: %lx\n", __func__, (unsigned long long)addr, data);
	#endif

	return cosim_mmio_write(cpd, addr, data, size);
}

static const MemoryRegionOps cpd_mmio_0_ops = {
    .read = cpd_mmio_0_read,
    .write = cpd_mmio_0_write,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
        .unaligned = false,
     },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};
    
static const MemoryRegionOps cpd_mmio_1_ops = {
    .read = cpd_mmio_1_read,
    .write = cpd_mmio_1_write,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
        .unaligned = false,
     },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static void cpd_realize(PCIDevice *pci_dev, Error **errp)
{
    CpdCtrl *cpd = CPD(pci_dev);
    unsigned table_size, pba_size;
	unsigned short nentries;
    uint8_t *pci_conf;


    pci_conf = pci_dev->config;
    pci_conf[PCI_COMMAND] = PCI_COMMAND_IO | PCI_COMMAND_MEMORY;

	/* connect to cocotb endpoint 
	 */
    if (cpd_connect_cocotb_endpoint(cpd, errp) < 0) {
        return;
    }

    memory_region_init(&cpd->bar0, OBJECT(cpd), "nvme-bar0", 32 * KiB);
    memory_region_init_io(&cpd->iomem, OBJECT(cpd), &cpd_mmio_0_ops, cpd, 
                        "nvme", 16 * KiB); 

    memory_region_add_subregion(&cpd->bar0, 0, &cpd->iomem);
    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY |
                        PCI_BASE_ADDRESS_MEM_TYPE_32, &cpd->bar0);

	/* exclusive bar for msix */
	nentries = 2048;
    table_size = nentries * PCI_MSIX_ENTRY_SIZE;
    pba_size = QEMU_ALIGN_UP(nentries, 64) / 8;

	memory_region_init(&cpd->msix_exclusive_bar, OBJECT(cpd), "nvme-msix", 16 * KiB);
	memory_region_init_io(&cpd->msix_table_mmio, OBJECT(cpd), &cpd_mmio_1_ops, cpd,
						"msix", table_size + pba_size);
	memory_region_add_subregion(&cpd->msix_exclusive_bar, 0, 
								&cpd->msix_table_mmio);

	pci_register_bar(pci_dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY |
						PCI_BASE_ADDRESS_MEM_TYPE_32, &cpd->msix_exclusive_bar);

    cpd->next_tag = 0;
    return;
}

static uint32_t cpd_config_read(PCIDevice *pci_dev, uint32_t address, int len) 
{
    CpdCtrl *cpd = CPD(pci_dev);
    uint32_t val;

    val = cosim_config_read(cpd, pci_get_bdf(pci_dev), address, len);

	#ifdef DEBUG
    printf("%s : address: %x val: %x len: %d\n", __func__, address, val, len);
	#endif
    return val;
}

static void cpd_config_write(PCIDevice *pci_dev, uint32_t address, uint32_t val, int len) 
{
    CpdCtrl *cpd = CPD(pci_dev);


    cosim_config_write(cpd, pci_get_bdf(pci_dev), address, val, len);

	/* call pci_default_write_config so memory regions can be enabled upon bar write
	 */
	pci_default_write_config(pci_dev, address, val, len);
}

static Property cpd_properties[] = {
    DEFINE_PROP_UINT16("np_port", CpdCtrl,  np_port, 5555),
    DEFINE_PROP_UINT16("push_port", CpdCtrl, push_port, 6000),
    DEFINE_PROP_UINT16("pull_port", CpdCtrl, pull_port, 6001),
    DEFINE_PROP_END_OF_LIST(),
};

static void cpd_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);

    pc->realize = cpd_realize;

    pc->vendor_id = 0x10ee;
    pc->device_id = 0x0100;
    pc->config_read = cpd_config_read;
    pc->config_write = cpd_config_write;

    device_class_set_props(dc, cpd_properties);
}

static const TypeInfo cpd_info = {
    .name   = TYPE_COCOTB_PCIE,
    .parent = TYPE_PCI_DEVICE,
    .instance_size  = sizeof(CpdCtrl),
    .class_init     = cpd_class_init,
    .interfaces     = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { },
    },
};

static void cpd_register(void)
{
    type_register_static(&cpd_info);
}

type_init(cpd_register);
