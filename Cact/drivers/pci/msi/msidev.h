#ifndef CACT_MSIDEV_H
#define CACT_MSIDEV_H

#include <stdint.h>
#include "pci_enum.h"

/*
 * Device interrupt layer.
 *
 * Owns the vector pool and the registration entry point drivers call.  MSI
 * and MSI-X are two ways of delivering the same thing — a LAPIC message
 * carrying a vector — so they share this pool, the IDT stubs and the dispatch
 * path.  The mechanism-specific programming lives in msi.c (capability ID
 * 0x05) and msix.c (capability ID 0x11).
 */

#define MSIDEV_VECTOR_BASE    0x30
#define MSIDEV_VECTOR_COUNT   192
#define MSIDEV_VECTOR_END     (MSIDEV_VECTOR_BASE + MSIDEV_VECTOR_COUNT)

void msidev_init(void);
int  msidev_alloc_vector(void);
void msidev_free_vector(int vector);
int  msidev_register_handler(int vector, void (*handler)(void));
void msidev_unregister_handler(int vector);
void msidev_dispatch(unsigned int vector);
int  msidev_used_vectors(void);

/*
 * Register @handler on the best interrupt mechanism @dev offers: MSI-X when it
 * is present and usable, otherwise a single MSI message.  Allocates a vector,
 * installs the handler and enables delivery.  Returns the vector (> 0), or -1
 * having left nothing allocated.
 */
int  msidev_register(pci_device_t *dev, void (*handler)(void));

/*
 * Release a vector handed out by msidev_register().  The device keeps its
 * MSI/MSI-X enable bit — it is re-pointed or torn down by its own driver.
 */
void msidev_unregister(int vector);

/* Re-arm the device-side state a resume cleared. */
void msidev_restore(void);

/*
 * Shared with msi.c/msix.c: walk the capability list for @cap_id (0 when it is
 * not there), and wake a function whose command register decodes nothing
 * before trusting what its capability list says.
 */
int  msidev_find_cap(pci_device_t *dev, uint8_t cap_id);
void msidev_ensure_enabled(pci_device_t *dev);

extern uint32_t msidev_stub_table[];

#endif
