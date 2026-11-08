#ifndef COCOTB_PCIE_COSIM_PORT_H
#define COCOTB_PCIE_COSIM_PORT_H

#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "qapi/error.h"

/*
 * Cosim port: the transport endpoint facing the external cocotb PCIe
 * model. 
 */
typedef struct CosimPort {
    /* ZMQ */
    void     *zmq_context;
    void     *np_socket;      /* REQ:  synchronous non-posted (cfg/mmio) */
    void     *push_socket;    /* PUSH: outgoing posted (mmio wr, DMA cpl) */
    void     *pull_socket;    /* PULL: incoming posted (DMA MRd/MWr)      */

    /* transport config */
    uint16_t  np_port;
    uint16_t  push_port;
    uint16_t  pull_port;

    uint8_t   next_tag;

    /* device used for pci_dma_read/write (set by the owning device) */
    PCIDevice *dma_dev;
} CosimPort;

int  cosim_port_connect(CosimPort *p, Error **errp);
void cosim_port_disconnect(CosimPort *p);

/* config space */
uint32_t cosim_config_read(CosimPort *p, uint16_t bdf, uint32_t addr,
                           int size, bool type1);
void     cosim_config_write(CosimPort *p, uint16_t bdf, uint32_t addr,
                            uint32_t val, int size, bool type1);

/* memory (BAR / window) */
uint64_t cosim_mmio_read(CosimPort *p, uint64_t addr, unsigned size);
void     cosim_mmio_write(CosimPort *p, uint64_t addr, uint64_t data,
                          unsigned size);

#endif /* COCOTB_PCIE_COSIM_PORT_H */
