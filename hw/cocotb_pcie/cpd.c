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
#include "cosim_port.h"
#include "cpd.h"
#include "qemu/log.h"

/* Implementation of Cocotb pcie device (cpd)
 * which talks to external simulation of PCIe endpoint on cocotb
 * Initially tested against nvme endpoint. But can talk to any flaveor of pci endpoint
 * NIC, GPU etc
 */

#define TYPE_COCOTB_PCIE "cocotb-pcie-endpoint"
#define CPD(obj)    OBJECT_CHECK(CpdCtrl, (obj), TYPE_COCOTB_PCIE)

/* BAR layout is fully configurable via device properties, so the SAME
 * binary works for any endpoint without recompilation. 
 * Example command lines:
 *
 *   # (BAR0 16 MiB, 64-bit, prefetchable; MSI-X inside BAR0)
 *   -device cocotb-pcie-endpoint,\
 *       bar0_size=16777216,bar0_type=1,bar0_prefetch=on
 *
 *   # (BAR0 16 KiB mem32; BAR1 16 KiB mem32 for MSI-X)
 *   -device cocotb-pcie-endpoint,\
 *       bar0_size=16384,bar0_type=0,\
 *       bar1_size=16384,bar1_type=0
 */
typedef struct CpdBarOpsCtx {
    CpdCtrl *cpd;
    uint8_t  bar_index;
} CpdBarOpsCtx;

static CpdBarOpsCtx cpd_bar_ctx_storage[CPD_MAX_BARS];

static uint64_t cpd_mmio_read(void *opaque, hwaddr offset, unsigned size)
{
    CpdBarOpsCtx *ctx = opaque;
    CpdCtrl *cpd;
    MemoryRegion *mr;
    hwaddr addr;

    if (!ctx || !ctx->cpd) {
        qemu_log("%s: improper rd access at offset %#" HWADDR_PRIx "\n",
                 __func__, offset);
        return 0;
    }

    cpd = ctx->cpd;
    mr  = &cpd->bar_mr[ctx->bar_index];

    addr = mr->addr + offset;

    return cosim_mmio_read(&cpd->port, addr, size);
}

static void cpd_mmio_write(void *opaque, hwaddr offset,
                           uint64_t data, unsigned size)
{
    CpdBarOpsCtx *ctx = opaque;
    CpdCtrl *cpd;
    MemoryRegion *mr;
    hwaddr addr;

    if (!ctx || !ctx->cpd) {
        qemu_log("%s: improper wr access at offset %#" HWADDR_PRIx "\n",
                 __func__, offset);
        return;
    }

    cpd = ctx->cpd;
    mr  = &cpd->bar_mr[ctx->bar_index];

    addr = mr->addr + offset;

#ifdef DEBUG
    printf("%s bar%u addr: %#llx data: %#lx size: %u\n", __func__,
           ctx->bar_index, (unsigned long long)addr, data, size);
#endif

    cosim_mmio_write(&cpd->port, addr, data, size);
}

static const MemoryRegionOps cpd_mmio_ops = {
    .read = cpd_mmio_read,
    .write = cpd_mmio_write,
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
    
static int cpd_register_bars(CpdCtrl *cpd, PCIDevice *pci_dev, Error **errp)
{
    int i = 0;

    while (i < CPD_MAX_BARS) {
        uint64_t size = cpd->bar_size[i];
        uint8_t  type = cpd->bar_type[i];
        uint8_t  pci_type;
        char     name[32];
        CpdBarOpsCtx *ctx;

        if (size == 0) {
            /* BAR not used */
            cpd->bar_present[i] = false;
            i++;
            continue;
        }

        /* Build the PCI BAR type flags */
        if (type == CPD_BAR_TYPE_IO) {
            pci_type = PCI_BASE_ADDRESS_SPACE_IO;
        } else {
            pci_type = PCI_BASE_ADDRESS_SPACE_MEMORY;
            if (type == CPD_BAR_TYPE_MEM64) {
                pci_type |= PCI_BASE_ADDRESS_MEM_TYPE_64;
            } else {
                pci_type |= PCI_BASE_ADDRESS_MEM_TYPE_32;
            }
            if (cpd->bar_prefetch[i]) {
                pci_type |= PCI_BASE_ADDRESS_MEM_PREFETCH;
            }
        }

        /* Per-BAR context so the handler knows its index */
        ctx = &cpd_bar_ctx_storage[i];
        ctx->cpd = cpd;
        ctx->bar_index = i;

        snprintf(name, sizeof(name), "cpd-bar%d", i);

        /* Entire BAR forwarded to cosim; no container/subregion needed */
        memory_region_init_io(&cpd->bar_mr[i], OBJECT(cpd), &cpd_mmio_ops,
                              ctx, name, size);
        pci_register_bar(pci_dev, i, pci_type, &cpd->bar_mr[i]);

        cpd->bar_present[i] = true;

        qemu_log("%s: registered BAR%d size=%#" PRIx64
                 " type=%s%s\n", __func__, i, size,
                 (type == CPD_BAR_TYPE_IO)    ? "io"    :
                 (type == CPD_BAR_TYPE_MEM64) ? "mem64" : "mem32",
                 cpd->bar_prefetch[i] ? " prefetch" : "");

        /* A 64-bit memory BAR consumes TWO BAR slots (i and i+1).
         * Skip the next index so we don't double-register. */
        if (type == CPD_BAR_TYPE_MEM64) {
            if (cpd->bar_size[i + 1] != 0) {
                error_setg(errp,
                    "BAR%d is 64-bit (consumes BAR%d); bar%d_size must be 0",
                    i, i + 1, i + 1);
                return -1;
            }
            cpd->bar_present[i + 1] = false;
            i += 2;
        } else {
            i += 1;
        }
    }

    return 0;
}

static void cpd_realize(PCIDevice *pci_dev, Error **errp)
{
    CpdCtrl *cpd = CPD(pci_dev);
    uint8_t *pci_conf;


    pci_conf = pci_dev->config;
    pci_conf[PCI_COMMAND] = PCI_COMMAND_IO | PCI_COMMAND_MEMORY;

    /* copy port config from device properties into the shared cosim port */
    cpd->port.np_port   = cpd->np_port;
    cpd->port.push_port = cpd->push_port;
    cpd->port.pull_port = cpd->pull_port;
    cpd->port.next_tag  = 0;
    cpd->port.dma_dev   = pci_dev;

    /* connect to cocotb endpoint
     */
    if (cosim_port_connect(&cpd->port, errp) < 0) {
        return;
    }

    /* register BARs from properties */
    if (cpd_register_bars(cpd, pci_dev, errp) < 0) {
        return;
    }

    return;
}

static uint32_t cpd_config_read(PCIDevice *pci_dev, uint32_t address, int len) 
{
    CpdCtrl *cpd = CPD(pci_dev);
    uint32_t val;

    val = cosim_config_read(&cpd->port, pci_get_bdf(pci_dev), address, len, false);

    #ifdef DEBUG
    printf("%s : address: %x val: %x len: %d\n", __func__, address, val, len);
    #endif
    return val;
}

static void cpd_config_write(PCIDevice *pci_dev, uint32_t address, uint32_t val, int len) 
{
    CpdCtrl *cpd = CPD(pci_dev);


    cosim_config_write(&cpd->port, pci_get_bdf(pci_dev), address, val, len, false);

    /* call pci_default_write_config so memory regions can be enabled upon bar write
     */
    pci_default_write_config(pci_dev, address, val, len);
}

static Property cpd_properties[] = {
    /* cosim transport */
    DEFINE_PROP_UINT16("np_port", CpdCtrl,  np_port, 5555),
    DEFINE_PROP_UINT16("push_port", CpdCtrl, push_port, 6000),
    DEFINE_PROP_UINT16("pull_port", CpdCtrl, pull_port, 6001),

    /* Per-BAR config: size (0 = unused), type (0=mem32,1=mem64,2=io),
     * prefetch (bool). */
    DEFINE_PROP_UINT64("bar0_size", CpdCtrl, bar_size[0], 0),
    DEFINE_PROP_UINT8 ("bar0_type", CpdCtrl, bar_type[0], CPD_BAR_TYPE_MEM32),
    DEFINE_PROP_BOOL  ("bar0_prefetch", CpdCtrl, bar_prefetch[0], false),

    DEFINE_PROP_UINT64("bar1_size", CpdCtrl, bar_size[1], 0),
    DEFINE_PROP_UINT8 ("bar1_type", CpdCtrl, bar_type[1], CPD_BAR_TYPE_MEM32),
    DEFINE_PROP_BOOL  ("bar1_prefetch", CpdCtrl, bar_prefetch[1], false),

    DEFINE_PROP_UINT64("bar2_size", CpdCtrl, bar_size[2], 0),
    DEFINE_PROP_UINT8 ("bar2_type", CpdCtrl, bar_type[2], CPD_BAR_TYPE_MEM32),
    DEFINE_PROP_BOOL  ("bar2_prefetch", CpdCtrl, bar_prefetch[2], false),

    DEFINE_PROP_UINT64("bar3_size", CpdCtrl, bar_size[3], 0),
    DEFINE_PROP_UINT8 ("bar3_type", CpdCtrl, bar_type[3], CPD_BAR_TYPE_MEM32),
    DEFINE_PROP_BOOL  ("bar3_prefetch", CpdCtrl, bar_prefetch[3], false),

    DEFINE_PROP_UINT64("bar4_size", CpdCtrl, bar_size[4], 0),
    DEFINE_PROP_UINT8 ("bar4_type", CpdCtrl, bar_type[4], CPD_BAR_TYPE_MEM32),
    DEFINE_PROP_BOOL  ("bar4_prefetch", CpdCtrl, bar_prefetch[4], false),

    DEFINE_PROP_UINT64("bar5_size", CpdCtrl, bar_size[5], 0),
    DEFINE_PROP_UINT8 ("bar5_type", CpdCtrl, bar_type[5], CPD_BAR_TYPE_MEM32),
    DEFINE_PROP_BOOL  ("bar5_prefetch", CpdCtrl, bar_prefetch[5], false),

    DEFINE_PROP_END_OF_LIST(),
};

static void cpd_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);

    pc->realize = cpd_realize;

    /* Note: config space is forwarded to cocotb, so these QEMU-side
     * IDs are mostly placeholders; the guest sees what the cocotb
     * device model reports. Set sane defaults anyway. */
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
