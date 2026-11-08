/*
 * cpd.h
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
#ifndef __COCOTB_PCIE_DEV_H_
#define __COCOTB_PCIE_DEV_H_

#include "hw/pci/pci_device.h"
#include "exec/memory.h"
#include "qapi/error.h"

#define CPD_MAX_BARS 6

enum {
    CPD_BAR_TYPE_MEM32 = 0,
    CPD_BAR_TYPE_MEM64 = 1,
    CPD_BAR_TYPE_IO    = 2,
};
typedef struct cocotb_pcie_ctrl_s {
    PCIDevice   parent_obj;
    MemoryRegion bar0;
    MemoryRegion iomem;
    MemoryRegion msix_exclusive_bar;
    MemoryRegion msix_table_mmio;
    MemoryRegion msix_pba_mmio;

    /* Per-BAR memory regions and config */
    MemoryRegion bar_mr[CPD_MAX_BARS];
    uint64_t     bar_size[CPD_MAX_BARS];
    uint8_t      bar_type[CPD_MAX_BARS];
    bool         bar_prefetch[CPD_MAX_BARS];
    bool         bar_present[CPD_MAX_BARS];

    /* Cosim stuff */
    CosimPort    port;

    /* Port configuration */
    uint16_t np_port;
    uint16_t push_port;
    uint16_t pull_port;
} CpdCtrl;

#endif
