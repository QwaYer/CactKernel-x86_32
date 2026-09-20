#ifndef CACT_MSIX_H
#define CACT_MSIX_H

#include <stdint.h>
#include "pci_enum.h"

/*
 * MSI-X (capability ID 0x11) capability programming.  Delivery itself goes
 * through the shared vector pool in msidev.h — this file only deals with the
 * table in device MMIO and the capability's own control word.
 */

#define MSIX_TABLE_ENTRY_SIZE   16
#define MSIX_VECTOR_CTRL_MASK   (1u << 0)

struct msix_table_entry {
    uint32_t msg_addr_lo;
    uint32_t msg_addr_hi;
    uint32_t msg_data;
    uint32_t vector_ctrl;
} __attribute__((packed));

struct msix_cap {
    uint16_t    cap_id;
    uint16_t    msg_ctrl;
    uint32_t    table_offset;
    uint32_t    pba_offset;
};

int  pci_msix_support(pci_device_t *dev);
int  pci_msix_table_map(pci_device_t *dev,
                        volatile struct msix_table_entry **table_out,
                        uint32_t *table_size_out);
int  pci_msix_pba_map(pci_device_t *dev,
                      volatile uint32_t **pba_out);
int  pci_msix_enable(pci_device_t *dev, int vector,
                     volatile struct msix_table_entry *table,
                     unsigned int entry_idx);

/*
 * Re-program the device-side MSI-X tables after a resume.
 *
 * The MSI-X enable bit travels with the configuration snapshot, but the table
 * itself lives in device MMIO and is cleared by the platform reset, so every
 * enabled entry has to be written again with the LAPIC address and vector.
 */
void msix_restore(void);

#endif
