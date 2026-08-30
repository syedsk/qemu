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

typedef struct cocotb_pcie_ctrl_s {
    PCIDevice   parent_obj;
    MemoryRegion bar0;
    MemoryRegion iomem;
    MemoryRegion msix_exclusive_bar;
    MemoryRegion msix_table_mmio;
    MemoryRegion msix_pba_mmio;

    /* Cosim stuff */
    void *zmq_context;
    void *np_socket;
	void *push_socket;
	void *pull_socket;

    uint8_t next_tag;
    /* Port configuration */
    uint16_t np_port;
    uint16_t push_port;
    uint16_t pull_port;
} CpdCtrl;

int cpd_connect_cocotb_endpoint(CpdCtrl *cpd, Error **errp);
uint32_t cosim_config_read(CpdCtrl *cpd, uint16_t bdf, uint32_t addr, int size);
void cosim_config_write(CpdCtrl *cpd, uint16_t bdf, uint32_t addr, uint32_t val, int size);
uint64_t cosim_mmio_read(CpdCtrl *cpd, uint64_t addr, unsigned size);
void cosim_mmio_write(CpdCtrl *cpd, uint64_t addr, uint64_t data, unsigned size);
#endif
