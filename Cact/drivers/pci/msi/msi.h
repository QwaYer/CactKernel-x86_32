#ifndef CACT_MSI_H
#define CACT_MSI_H

#include <stdint.h>
#include <stdbool.h>
#include "pci_enum.h"

#define MSIX_VECTOR_BASE    0x30
#define MSIX_VECTOR_COUNT   192
#define MSIX_VECTOR_END     (MSIX_VECTOR_BASE + MSIX_VECTOR_COUNT)

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

void msix_init(void);
int  msix_alloc_vector(void);
void msix_free_vector(int vector);
int  msix_register_handler(int vector, void (*handler)(void));
void msix_unregister_handler(int vector);
void msix_dispatch(unsigned int vector);

int  pci_msix_support(pci_device_t *dev);
int  pci_msix_enable(pci_device_t *dev, int vector,
                     volatile struct msix_table_entry *table,
                     unsigned int entry_idx);
int  pci_msix_table_map(pci_device_t *dev,
                        volatile struct msix_table_entry **table_out,
                        uint32_t *table_size_out);
int  pci_msix_pba_map(pci_device_t *dev,
                      volatile uint32_t **pba_out);

int msix_used_vectors(void);

extern uint32_t msix_stub_table[];

#endif
