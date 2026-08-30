/*
 * cosim_port.c
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

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <zmq.h>

#include "qapi/error.h"
#include "hw/pci/pci.h"
#include "qemu/log.h"
#include "cpd.h"

/* TLP Format and Type definitions */
#define PCIE_FMT_3DW_NO_DATA    0x0  /* 3DW header, no data */
#define PCIE_FMT_4DW_NO_DATA    0x1  /* 4DW header, no data */
#define PCIE_FMT_3DW_WITH_DATA  0x2  /* 3DW header, with data */
#define PCIE_FMT_4DW_WITH_DATA  0x3  /* 4DW header, with data */

#define PCIE_TYPE_MRD			0x00  /* Memory Read */
#define PCIE_TYPE_MRDLK			0x01  /* Memory Read Lock */
#define PCIE_TYPE_MWR	        0x00  /* Memory Write (with data) */
#define PCIE_TYPE_CFG_READ_0    0x04 /* Configuration Read Type 0 */
#define PCIE_TYPE_CFG_WRITE_0   0x04 /* Configuration Write Type 0 */
#define PCIE_TYPE_CFG_READ_1    0x05 /* Configuration Read Type 1 */
#define PCIE_TYPE_CFG_WRITE_1   0x05 /* Configuration Write Type 1 */
#define PCIE_TYPE_CPL           0x0A /* Completion without data */
#define PCIE_TYPE_CPL_D         0x0A /* Completion with data */

/* Completion Status */
#define CPL_STATUS_SC       0x0     /* Successful Completion */
#define CPL_STATUS_UR       0x1     /* Unsupported Request */
#define CPL_STATUS_CRS      0x2     /* Configuration Request Retry Status */
#define CPL_STATUS_CA       0x4     /* Completer Abort */

#define MAX_PAYLOAD_SIZE 4096

/* Memory Request Header (32-bit address, 3 DW) */
typedef struct __attribute__((packed)) {
    uint8_t fmt_type;
    uint8_t tc_attr_th;      /* TC, Attr, TH */
    uint16_t length;         /* Length in DWords */
    uint16_t requester_id;
    uint8_t tag;
    uint8_t be;
    uint32_t address;        /* 32-bit address (DW aligned) */
} PCIeTLPMemReq32;

/* Memory Request Header (64-bit address, 4 DW) */
typedef struct __attribute__((packed)) {
    uint8_t fmt_type;
    uint8_t tc_attr_th;
    uint16_t length;
    uint16_t requester_id;
    uint8_t tag;
    uint8_t be;
    uint64_t address;        /* 64-bit address (DW aligned) */
} PCIeTLPMemReq64;

typedef struct __attribute__((packed)) {
    uint8_t fmt_type;
    uint8_t tc_attr_th;      /* TC, Attr, TH */
    uint16_t length;         /* Length in DWords */
    uint16_t completer_id;
	uint16_t cs_byte_cnt;
    uint16_t requester_id;
	uint8_t tag;
	uint8_t lower_addr;
} PCIeTLPCpl;

static void handle_posted_ops(void *opaque);

int cpd_connect_cocotb_endpoint(CpdCtrl *cpd, Error **errp) 
{
	char endpoint[256];
	int rc;
	int zmq_pull_fd;

	cpd->zmq_context = zmq_ctx_new();
	if (!cpd->zmq_context) {
        error_setg(errp, "Failed to create ZMQ context");
        return -1;
	}

	/* for non-posted operations
	 */
	cpd->np_socket = zmq_socket(cpd->zmq_context, ZMQ_REQ);
	if (!cpd->np_socket) {
        error_setg(errp, "Failed to create ZMQ NP socket");
		goto err_np;
	}

	/* for outgoing posted operations
	 */
	cpd->push_socket = zmq_socket(cpd->zmq_context, ZMQ_PUSH);
	if (!cpd->push_socket) {
        error_setg(errp, "Failed to create ZMQ push socket");
		goto err_push;
	}

	/* for incoming posted operations
	 */
	cpd->pull_socket = zmq_socket(cpd->zmq_context, ZMQ_PULL);
	if (!cpd->pull_socket) {
        error_setg(errp, "Failed to create ZMQ pull socket");
		goto err_pull;
	}

	snprintf(endpoint, sizeof(endpoint), "tcp://localhost:%d", cpd->np_port);
	rc = zmq_connect(cpd->np_socket, endpoint);
	if (rc != 0) {
        error_setg(errp, "Failed to connect ZMQ NP socket %s: %s",
                   endpoint, zmq_strerror(errno));
		goto err_conn;
	}

	snprintf(endpoint, sizeof(endpoint), "tcp://localhost:%d", cpd->push_port);
	rc = zmq_connect(cpd->push_socket, endpoint);
	if (rc != 0) {
        error_setg(errp, "Failed to connect ZMQ push socket %s: %s",
                   endpoint, zmq_strerror(errno));
		goto err_conn;
	}

	snprintf(endpoint, sizeof(endpoint), "tcp://localhost:%d", cpd->pull_port);
	rc = zmq_connect(cpd->pull_socket, endpoint);
	if (rc != 0) {
        error_setg(errp, "Failed to bind to ZMQ pull socket %s: %s",
                   endpoint, zmq_strerror(errno));
		goto err_conn;
	}

    size_t opt_len = sizeof(int);
	rc = zmq_getsockopt(cpd->pull_socket, ZMQ_FD, &zmq_pull_fd, &opt_len);
	if (rc != 0) {
        error_setg(errp, "Failed to set sockopt on pull socket %s: %s",
                   endpoint, zmq_strerror(errno));
		goto err_conn;
	}
	
	qemu_set_fd_handler(zmq_pull_fd, handle_posted_ops, NULL, cpd);

	return 0;

err_conn:
	zmq_close(cpd->pull_socket);
err_pull:
    zmq_close(cpd->push_socket);
err_push:
    zmq_close(cpd->np_socket);
err_np:
    zmq_ctx_destroy(cpd->zmq_context);
	return -1;
}

static void calculate_byte_enables(uint32_t addr, unsigned size,
                                   uint8_t *first_dw_be, uint8_t *last_dw_be)
{
    uint32_t offset = addr & 0x3;  /* Offset within the DWORD */
    uint32_t dword_count = ((offset + size + 3) / 4);  /* Number of DWORDs */
    
    /* Calculate first DWORD byte enable */
    *first_dw_be = 0;
    for (unsigned i = 0; i < size && i < (4 - offset); i++) {
        *first_dw_be |= (1 << (offset + i));
    }
    
    /* Calculate last DWORD byte enable */
    if (dword_count == 1) {
        *last_dw_be = 0;  /* Single DWORD - only first_dw_be is used */
    } else {
        *last_dw_be = 0;
        uint32_t remaining = size - (4 - offset);
        uint32_t last_dword_bytes = remaining % 4;
        if (last_dword_bytes == 0) last_dword_bytes = 4;
        
        for (unsigned i = 0; i < last_dword_bytes; i++) {
            *last_dw_be |= (1 << i);
        }
    }
}

static size_t generate_config_read_tlp(uint8_t *buffer, size_t buffer_size,
                                       uint16_t requester_id, uint8_t tag,
									   uint16_t bdf,
                                       uint16_t reg, unsigned size)
{
    if (buffer_size < 12) {  /* 3DW = 12 bytes */
        return 0;
    }
    
    uint8_t first_dw_be, last_dw_be;
    calculate_byte_enables(reg, size, &first_dw_be, &last_dw_be);
    
#ifdef DEBUG
	printf("generate_config_read_tlp: first_be: %x last_be: %x\n", first_dw_be, last_dw_be);
#endif

    uint32_t length = 1;
    
    /* DW0: Format (3 bits) + Type (5 bits) + T9 + Attr + Length */
    buffer[0] = (PCIE_FMT_3DW_NO_DATA << 5) | PCIE_TYPE_CFG_READ_0;
    buffer[1] = 0x00;  /* T9 = 0 (TC=0), Attr = 0 */
    buffer[2] = (length >> 8) & 0x03;  /* Length high bits */
    buffer[3] = length & 0xFF;         /* Length low bits */
    
    /* DW1: Requester ID + Tag + Byte Enables */
    buffer[4] = (requester_id >> 8) & 0xFF;
    buffer[5] = requester_id & 0xFF;
    buffer[6] = tag;
    buffer[7] = (last_dw_be << 4) | first_dw_be;
    
    /* DW2: BDF (Bus/Device/Function) + Register Address */
    uint32_t bdf_reg = (bdf << 16) | (reg & 0xFFC);
    buffer[8] = (bdf_reg >> 24) & 0xFF;
    buffer[9] = (bdf_reg >> 16) & 0xFF;
    buffer[10] = (bdf_reg >> 8) & 0xFF;
    buffer[11] = bdf_reg & 0xFF;
    
    return 12;  /* 3DW header */
}

static size_t generate_config_write_tlp(uint8_t *buffer, size_t buffer_size,
                                        uint16_t requester_id, uint8_t tag,
										uint16_t bdf,
                                        uint16_t reg, uint32_t val, unsigned size)
{
    uint8_t first_dw_be, last_dw_be;
    calculate_byte_enables(reg, size, &first_dw_be, &last_dw_be);
    uint32_t offset = reg & 0x3;
    
    uint32_t length = 1;
    
    size_t total_size = 12 + (length * 4);  /* 3DW header + payload */
    if (buffer_size < total_size) {
        return 0;
    }
    
    /* DW0: Format (3 bits) + Type (5 bits) + T9 + Attr + Length */
    buffer[0] = (PCIE_FMT_3DW_WITH_DATA << 5) | PCIE_TYPE_CFG_WRITE_0;
    buffer[1] = 0x00;  /* T9 = 0 (TC=0), Attr = 0 */
    buffer[2] = (length >> 8) & 0x03;  /* Length high bits */
    buffer[3] = length & 0xFF;         /* Length low bits */
    
    /* DW1: Requester ID + Tag + Byte Enables */
    buffer[4] = (requester_id >> 8) & 0xFF;
    buffer[5] = requester_id & 0xFF;
    buffer[6] = tag;
    buffer[7] = (last_dw_be << 4) | first_dw_be;
    
    /* DW2: BDF (Bus/Device/Function) + Register Address */
    uint32_t bdf_reg = (bdf << 16) | (reg & 0xFFC);
    buffer[8] = (bdf_reg >> 24) & 0xFF;
    buffer[9] = (bdf_reg >> 16) & 0xFF;
    buffer[10] = (bdf_reg >> 8) & 0xFF;
    buffer[11] = bdf_reg & 0xFF;
    
    /* DW3+: Data Payload - align data to DWORD boundary */
    memset(&buffer[12], 0, length * 4);  /* Zero out payload area */
    
    /* Copy data at the correct byte offset within the DWORD-aligned buffer */
    uint8_t *payload_ptr = &buffer[12 + offset];
    memcpy(payload_ptr, &val, size);
    
    return total_size;
}


static uint64_t parse_completion_tlp(const uint8_t *buffer, size_t buffer_size,
                                     uint8_t expected_tag, uint8_t byte_offset, unsigned size)
{
	PCIeTLPCpl *cpl = (PCIeTLPCpl *)buffer;

    if (buffer_size < 12) {  /* Minimum 3DW header */
        return 0;
    }

	if ((cpl->fmt_type & 0x1f) != PCIE_TYPE_CPL_D) {
        fprintf(stderr, "%s: Not a completion with data\n", __func__);
        return 0;  
	}

	if (cpl->tag != expected_tag) {
        fprintf(stderr, "%s: Tag mismatch\n", __func__);
        return 0;  
    }

	uint8_t cs = (cpl->cs_byte_cnt >> 5) & 0x7;
    if (cs != 0) {  
        fprintf(stderr, "%s: Comletion error status:%x \n", __func__, cs);
        return 0;  
    }

    /* Extract data from payload (DW3+) */
    uint64_t result = 0;
    if (buffer_size >= 16) {
        const uint8_t *data_ptr = &buffer[12 + byte_offset];
        for (unsigned i = 0; i < size; i++) 
            result |= ((uint64_t)data_ptr[i]) << (i * 8);
    }

	return result;
}

#if 0
static uint64_t parse_completion_tlp(const uint8_t *buffer, size_t buffer_size,
                                     uint8_t expected_tag, unsigned size)
{
    if (buffer_size < 12) {  /* Minimum 3DW header */
        return 0;
    }
    
    /* Verify it's a completion */
    //uint8_t fmt = (buffer[0] >> 5) & 0x7;
    uint8_t type = buffer[0] & 0x1F;
    
    if (type != PCIE_TYPE_CPL_D) {
        fprintf(stderr, "%s: Not a completion with data\n", __func__);
        return 0;  
    }
    
    /* Extract tag from DW2 */
    uint8_t tag = buffer[10];
    if (tag != expected_tag) {
        fprintf(stderr, "%s: Tag mismatch\n", __func__);
        return 0;  
    }
    
    /* Extract status from DW1 */
    uint8_t status = (buffer[6] >> 5) & 0x7;
    if (status != 0) {  /* 0 = Successful Completion */
        fprintf(stderr, "%s: Comletion error status:%x \n", __func__, status);
        return 0;  /* Completion with error */
    }
    
    /* Extract lower address to determine byte offset */
    uint8_t lower_addr = buffer[11] & 0x7F;
	uint8_t byte_offset = lower_addr & 0x3;

	printf("parse_completion_tlp: lower_address: %x\n", lower_addr);
    
    /* Extract data from payload (DW3+) */
    uint64_t result = 0;
    if (buffer_size >= 16) {
        const uint8_t *data_ptr = &buffer[12 + byte_offset];
        for (unsigned i = 0; i < size; i++) 
            result |= ((uint64_t)data_ptr[i]) << (i * 8);
    }
    
    return result;
}
#endif

uint32_t cosim_config_read(CpdCtrl *cpd, uint16_t bdf, uint32_t addr, int size)
{
    uint8_t tlp_buffer[256];
    uint8_t response_buf[256];
    
    /* Get a unique tag for this transaction */
    uint8_t tag = cpd->next_tag++;
    
    /* Generate Configuration Read TLP */
    size_t tlp_size = generate_config_read_tlp(
        tlp_buffer, sizeof(tlp_buffer),
        0, tag,
		bdf,
        addr, size
    );
    
    if (tlp_size == 0) {
        fprintf(stderr, "Failed to generate Config Read TLP\n");
        return 0xFFFFFFFF;
    }
    
#ifdef DEBUG
	{
		printf("config read request dump\n");
		for (int i = 0; i < tlp_size; i++)
			printf(" %x", tlp_buffer[i]);
		printf("\n");
	}
#endif
    /* Send TLP over ZMQ socket */
    int rc = zmq_send(cpd->np_socket, tlp_buffer, tlp_size, 0);
    if (rc < 0) {
        fprintf(stderr, "ZMQ send failed: %s\n", zmq_strerror(errno));
        return 0xFFFFFFFF;
    }

    /* Receive Completion TLP */
    rc = zmq_recv(cpd->np_socket, response_buf, sizeof(response_buf), 0);
    if (rc < 0) {
        fprintf(stderr, "ZMQ recv failed: %s\n", zmq_strerror(errno));
        return 0xFFFFFFFF;
    }
    
#ifdef DEBUG
	{
		printf("config read completion dump\n");
		for (int i = 0; i < rc; i++)
			printf(" %x", response_buf[i]);
		printf("\n");
	}
#endif

    /* Parse completion and extract data */
    uint32_t result = parse_completion_tlp(response_buf, rc, tag, (addr & 3), size);
    
#ifdef DEBUG
	printf("%s: addr: %x val: %x\n", __func__, addr, result);
#endif
    return result;
}

void cosim_config_write(CpdCtrl *cpd, uint16_t bdf, uint32_t addr, uint32_t val, int size)
{
    uint8_t tlp_buffer[256];
    uint8_t response_buffer[256];
    
#ifdef DEBUG
	printf("%s: addr: %x\n", __func__, addr);
#endif
    /* Get a unique tag for this transaction */
    uint8_t tag = cpd->next_tag++;
    
    /* Generate Configuration Write TLP */
    size_t tlp_size = generate_config_write_tlp(
        tlp_buffer, sizeof(tlp_buffer),
        0, tag,
        bdf,
        addr, val, size);
    
    if (tlp_size == 0) {
        fprintf(stderr, "Failed to generate Config Write TLP\n");
        return;
    }
    
    /* Send TLP over non-posted socket */
    int rc = zmq_send(cpd->np_socket, tlp_buffer, tlp_size, 0);
    if (rc < 0) {
        fprintf(stderr, "ZMQ send failed: %s\n", zmq_strerror(errno));
        return;
    }
    
    /* Receive Completion TLP (without data for writes) */
    rc = zmq_recv(cpd->np_socket, response_buffer, sizeof(response_buffer), 0);
    if (rc < 0) {
        fprintf(stderr, "ZMQ recv failed: %s\n", zmq_strerror(errno));
        return;
    }
    
    /* Verify completion status */
    if (rc >= 12) {
        uint8_t status = (response_buffer[6] >> 5) & 0x7;
        if (status != 0) {
            fprintf(stderr, "Config write completed with error status: %d\n", status);
        }
    }
}

uint64_t cosim_mmio_read(CpdCtrl *cpd, uint64_t addr, unsigned size)
{
	unsigned dw_length = (size + 3)/4;
	u_int8_t tlp_hdr[64];
    uint8_t response_buffer[256];
    uint8_t first_be, last_be;
    /* Get a unique tag for this transaction */
    uint8_t tag = cpd->next_tag++;

    calculate_byte_enables(addr, size, &first_be, &last_be);

	#ifdef DEBUG
	printf("cosim_mmio_read: first_be: %x last_be: %x\n", first_be, last_be);
	#endif

    bool is_64bit = (addr >> 32) != 0;
	uint8_t fmt = is_64bit ? PCIE_FMT_4DW_NO_DATA : PCIE_FMT_3DW_NO_DATA;
    unsigned hdr_size = is_64bit ? 16 : 12;

    PCIeTLPMemReq32 *hdr = (PCIeTLPMemReq32 *)tlp_hdr;
    hdr->fmt_type = ((fmt << 5) | PCIE_TYPE_MRD);
    hdr->tc_attr_th = 0;
    hdr->length = cpu_to_be16(dw_length & 0x3ff);
    hdr->requester_id = cpu_to_be16(0x0000);
    hdr->tag = tag;
	hdr->be = (last_be << 4) | first_be;

    if (is_64bit) {
        PCIeTLPMemReq64 *req = (PCIeTLPMemReq64 *)tlp_hdr;
        req->address = cpu_to_be64(addr & ~0x3ULL);
    } else {
        hdr->address = cpu_to_be32((uint32_t)(addr & ~0x3ULL));
    }

    /* Send over non-posted socket */
    int rc = zmq_send(cpd->np_socket, tlp_hdr, hdr_size, 0);
    if (rc < 0) {
        fprintf(stderr, "ZMQ send failed: %s\n", zmq_strerror(errno));
        return 0xFFFFFFFF;
    }

    /* Receive Completion TLP */
    rc = zmq_recv(cpd->np_socket, response_buffer, sizeof(response_buffer), 0);
    if (rc < 0) {
        fprintf(stderr, "ZMQ recv failed: %s\n", zmq_strerror(errno));
        return 0xFFFFFFFF;
    }
#ifdef DEBUG
	{
		printf("mmio read completion tlp dump\n");
		for (int i = 0; i < rc; i++)
			printf(" %x", response_buffer[i]);
		printf("\n");
	}   
#endif
    /* Parse completion and extract data */
    uint64_t result = parse_completion_tlp(response_buffer, rc, tag, (addr & 0x3), size);
    
    printf("%s addr: %llx, size: %d result: %lx\n", __func__, (unsigned long long)addr, size, result);
    return result;
}

void cosim_mmio_write(CpdCtrl *cpd, uint64_t addr, uint64_t data, unsigned size)
{
    unsigned dw_length = (size + 3) / 4; 
    uint8_t tlp_buf[64]; // Increased size for safety
    uint8_t first_be, last_be;
    uint8_t tag = cpd->next_tag++;

    calculate_byte_enables(addr, size, &first_be, &last_be);

    bool is_64bit = (addr >> 32) != 0;
    uint8_t fmt = is_64bit ? PCIE_FMT_4DW_WITH_DATA : PCIE_FMT_3DW_WITH_DATA;
    unsigned hdr_size = is_64bit ? 16 : 12;

    PCIeTLPMemReq32 *hdr = (PCIeTLPMemReq32 *)tlp_buf;
    hdr->fmt_type = (fmt << 5) | PCIE_TYPE_MWR;
    hdr->tc_attr_th = 0;
    hdr->length = cpu_to_be16(dw_length & 0x3FF);
    hdr->requester_id = cpu_to_be16(0x0000);
    hdr->tag = tag;
    hdr->be = (last_be << 4) | first_be;

    if (is_64bit) {
        PCIeTLPMemReq64 *req = (PCIeTLPMemReq64 *)tlp_buf;
        req->address = cpu_to_be64(addr & ~0x3ULL);
    } else {
        hdr->address = cpu_to_be32((uint32_t)(addr & ~0x3ULL));
    }

    uint8_t *payload = tlp_buf + hdr_size;
    memset(payload, 0, dw_length * 4);
    for (unsigned i = 0; i < size; i++) {
        payload[i] = (data >> (i * 8)) & 0xFF;
    }

    zmq_send(cpd->push_socket, tlp_buf, hdr_size + (dw_length * 4), 0);
}

static void cosim_build_completion(u_int8_t *hdr, 
								   uint16_t requester_id, 
								   uint8_t tag, 
								   uint16_t payload_len, 
								   uint64_t address)
{
	PCIeTLPCpl *cpl = (PCIeTLPCpl *)hdr;
	uint16_t cs_byte_cnt;

	if (payload_len > 0) 
		cpl->fmt_type = (PCIE_FMT_3DW_WITH_DATA << 5) | PCIE_TYPE_CPL_D;
	else 
		cpl->fmt_type = (PCIE_FMT_3DW_NO_DATA << 5) | PCIE_TYPE_CPL;

	cpl->tc_attr_th = 0;

	uint16_t length_dw = (payload_len + 3)/4;
	cpl->length = cpu_to_be16(length_dw & 0x3ff);
	cs_byte_cnt = (CPL_STATUS_SC << 12);
	cs_byte_cnt |= payload_len & 0xfff;
	cpl->cs_byte_cnt = cpu_to_be16(cs_byte_cnt);
	cpl->tag = tag;
	cpl->completer_id = cpu_to_be16(0x0);
	cpl->requester_id = cpu_to_be16(requester_id);
	cpl->lower_addr = address & 0x7f;
}

static void cosim_handle_mrd(CpdCtrl *cpd, uint8_t *buf)
{
    PCIDevice *pci_dev = PCI_DEVICE(cpd);
	uint8_t *data_buf;
    MemTxResult result;
	/* mps + 12byte cpl header */
    uint8_t response_buf[MAX_PAYLOAD_SIZE + 12];
	uint8_t tag;
	uint64_t address;
	uint16_t length, requester_id;

	PCIeTLPMemReq32 *req = (PCIeTLPMemReq32 *)buf;
	length = be16_to_cpu(req->length) & 0x3ff;
	requester_id = be16_to_cpu(req->requester_id);
	tag = req->tag;


	/* 3DW memrd */
	if (buf[0] == 0x0) {
		address = be32_to_cpu(req->address);
	} else {
		/* 4dw mem read */
		PCIeTLPMemReq64 *req64 = (PCIeTLPMemReq64 *)buf;
		address = be64_to_cpu(req64->address);
	}

#ifdef DEBUG
    fprintf(stderr, "cosim_handle_mrd address 0x%lx, len=%u, tag=%d\n", address, length, tag);
#endif

	if (length == 0)
		length = 1024 * 4; /* 1024 DW */
	else
		length = length * 4;


    /* Use PCI DMA API to read from system memory */
	/* TODO check mps
	 */
	data_buf = response_buf + sizeof(PCIeTLPCpl);
    result = pci_dma_read(pci_dev, address, data_buf, length);
    if (result != MEMTX_OK) {
        fprintf(stderr, "PCIe: DMA read failed at 0x%lx, len=%u, result=%d\n",
                address, length, result);
		/* TODO send completion for error 
		 */
		return;
    }
	/* TODO add support for split completions
	 */
	uint32_t cpl_len = length + 12;
	cosim_build_completion(response_buf,
						   requester_id,
						   tag,
						   length,
						   address);

#ifdef DEBUG
	{
		printf("mmio read completion dump\n");
		for (int i = 0; i < cpl_len; i++)
			printf(" %x", response_buf[i]);
		printf("\n");
	}
#endif
	zmq_send(cpd->push_socket, response_buf, cpl_len, 0);

	return;
}

static void cosim_handle_mwr(CpdCtrl *cpd, uint8_t *buf)
{
    PCIDevice *pci_dev = PCI_DEVICE(cpd);
    MemTxResult result;
	uint8_t tag;
	uint64_t address;
	uint16_t length, requester_id;
	uint8_t *data_buf;
	
	PCIeTLPMemReq32 *req = (PCIeTLPMemReq32 *)buf;
	length = be16_to_cpu(req->length) & 0x3ff;
	requester_id = be16_to_cpu(req->requester_id);
	tag = req->tag;

	(void)tag;
	(void)requester_id;

	if (buf[0] == 0x40) {
		address = be32_to_cpu(req->address);
		data_buf = buf + sizeof(PCIeTLPMemReq32);
	} else {
		/* 4dw mem read */
		PCIeTLPMemReq64 *req64 = (PCIeTLPMemReq64 *)buf;
		address = be64_to_cpu(req64->address);
		data_buf = buf + sizeof(PCIeTLPMemReq64);
	}

	if (length == 0)
		length = 1024 * 4; /* 1024 DW */
	else
		length = length * 4;

    /* Use PCI DMA API to write to system memory */
	/* TODO handle mps 
	 */
    result = pci_dma_write(pci_dev, address, data_buf, length);
    if (result != MEMTX_OK) {
        fprintf(stderr, "PCIe: DMA read failed at 0x%lx, len=%u, result=%d\n",
                address, length, result);
		return;
    }
}

static void handle_posted_ops(void *opaque) 
{
	CpdCtrl *cpd = (CpdCtrl *)opaque;
	uint32_t events;
    size_t len = sizeof(events);

	while (1) {
		int rc = zmq_getsockopt(cpd->pull_socket, ZMQ_EVENTS, &events, &len);
		if (rc < 0) {
			fprintf(stderr, "ZMQ get sock opt failed: %s\n", zmq_strerror(errno));
			return;
		}

		if (!(events & ZMQ_POLLIN)) 
			break;

		if (events & ZMQ_POLLIN) {
			uint8_t buf[MAX_PAYLOAD_SIZE + 16];

			int nbytes = zmq_recv(cpd->pull_socket, buf, sizeof(buf), ZMQ_DONTWAIT);
			if (nbytes < 0) {
				fprintf(stderr, "ZMQ recv failed: %s\n", zmq_strerror(errno));
				return;
			}
			if (buf[0] == 0x40 || buf[0] == 0x60) {
				/* incoming memory write */
				cosim_handle_mwr(cpd, buf);
			} else if (buf[0] == 0x0 || buf[0] == 0x20) {
				/* incoming memory read */
				cosim_handle_mrd(cpd, buf);
			}
		}
	}

}
