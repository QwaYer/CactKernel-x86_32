#include "msidev.h"
#include "msi.h"
#include "pci.h"
#include "pcidev.h"
#include "kernel.h"
#include "idt.h"
#include "klib.h"

static void (*msidev_handlers[MSIDEV_VECTOR_COUNT])(void);
static unsigned char msidev_vector_alloc[MSIDEV_VECTOR_COUNT];
static int msidev_initialized = 0;

void msidev_init(void)
{
    memset(msidev_handlers, 0, sizeof(msidev_handlers));
    memset(msidev_vector_alloc, 0, sizeof(msidev_vector_alloc));

    /* Vector 0x80 is not allocatable: it is the int-0x80 syscall gate and
     * must never be overwritten by a device interrupt stub. */
    unsigned int syscall_idx = 0x80 - MSIDEV_VECTOR_BASE;
    if (syscall_idx < MSIDEV_VECTOR_COUNT)
        msidev_vector_alloc[syscall_idx] = 1;

    msidev_initialized = 1;
    pr_info("  %-11s : vector pool ready\n", "msidev");
}

int msidev_alloc_vector(void)
{
    if (!msidev_initialized) return -1;
    for (unsigned int i = 0; i < MSIDEV_VECTOR_COUNT; i++) {
        if (!msidev_vector_alloc[i]) {
            msidev_vector_alloc[i] = 1;
            return MSIDEV_VECTOR_BASE + i;
        }
    }
    pr_warn("  %-11s : no free vectors\n", "msidev");
    return -1;
}

void msidev_free_vector(int vector)
{
    if (vector < MSIDEV_VECTOR_BASE || vector >= MSIDEV_VECTOR_END) return;
    unsigned int idx = vector - MSIDEV_VECTOR_BASE;
    msidev_vector_alloc[idx] = 0;
    msidev_handlers[idx] = NULL;
}

int msidev_register_handler(int vector, void (*handler)(void))
{
    if (!msidev_initialized) return -1;
    if (vector < MSIDEV_VECTOR_BASE || vector >= MSIDEV_VECTOR_END) return -1;
    if (!handler) return -1;

    unsigned int idx = vector - MSIDEV_VECTOR_BASE;
    if (msidev_handlers[idx]) return -1;
    msidev_handlers[idx] = handler;
    set_idt_gate(vector, msidev_stub_table[idx]);
    return 0;
}

void msidev_unregister_handler(int vector)
{
    if (vector < MSIDEV_VECTOR_BASE || vector >= MSIDEV_VECTOR_END) return;
    unsigned int idx = vector - MSIDEV_VECTOR_BASE;
    msidev_handlers[idx] = NULL;
}

void msidev_dispatch(unsigned int vector)
{
    if (vector < MSIDEV_VECTOR_BASE || vector >= MSIDEV_VECTOR_END) return;
    unsigned int idx = vector - MSIDEV_VECTOR_BASE;
    void (*h)(void) = msidev_handlers[idx];
    if (h) h();
}

int msidev_used_vectors(void)
{
    int count = 0;
    for (unsigned int i = 0; i < MSIDEV_VECTOR_COUNT; i++)
        if (msidev_vector_alloc[i]) count++;
    return count;
}

/* Walk the standard capability list for cap_id, returning its offset or 0.
 * Shared by the MSI and MSI-X layers. */
int msidev_find_cap(pci_device_t *dev, uint8_t cap_id)
{
    if (!dev) return 0;

    uint8_t p = pcidev_cfg_read8(dev, 0x34);
    for (int i = 0; i < 48 && p >= 0x40 && p != 0xFF; i++) {
        if (pcidev_cfg_read8(dev, p) == cap_id) return (int)p;
        p = pcidev_cfg_read8(dev, (uint16_t)(p + 1));   /* NextPtr */
    }
    return 0;
}

/* Wake a function that enumeration found but nobody has enabled yet: one whose
 * command register decodes nothing can report an empty capability list (or
 * stop answering entirely), which is indistinguishable from a device that has
 * neither MSI nor MSI-X.  Idempotent — the bits are only written when they are
 * actually missing. */
void msidev_ensure_enabled(pci_device_t *dev)
{
    if (!dev) return;

    uint32_t cmd = pcidev_cfg_read32(dev, 0x04);
    if (cmd == 0xFFFFFFFFu) return;                     /* config not readable */

    uint32_t want = cmd | (1u << 1) | (1u << 2);        /* MEM_SPACE | BUS_MASTER */
    if (want == cmd) return;

    pcidev_cfg_write32(dev, 0x04, want);
    pr_info("  %-11s : %02x:%02x.%u command 0x%04x -> 0x%04x (memory+master on)\n",
            "msidev", (unsigned)dev->bus, (unsigned)dev->dev, (unsigned)dev->fn,
            (unsigned)cmd, (unsigned)want);
}

int msidev_register(pci_device_t *dev, void (*handler)(void))
{
    if (!dev || !handler) return -1;

    msidev_ensure_enabled(dev);

    /* MSI-X first: a table we control plus per-vector masking.  Going through
     * pci_msix_table_map() rather than pci_msix_support() means one
     * capability lookup, not two, so a device without MSI-X dumps its chain
     * once instead of on every step. */
    volatile struct msix_table_entry *table = NULL;
    uint32_t table_size = 0;
    if (pci_msix_table_map(dev, &table, &table_size) == 0 && table_size) {
        int vec = msidev_alloc_vector();
        if (vec > 0 && msidev_register_handler(vec, handler) == 0 &&
            pci_msix_enable(dev, vec, table, 0) == 0) {
            pr_info("  %-11s : %02x:%02x.%u on MSI-X vec 0x%x\n",
                    "msidev", (unsigned)dev->bus, (unsigned)dev->dev,
                    (unsigned)dev->fn, (unsigned)vec);
            return vec;
        }
        if (vec > 0) {
            msidev_unregister_handler(vec);
            msidev_free_vector(vec);
        }
    }

    /* Fall back to a single MSI message.  Controllers such as Intel PCH xHCI
     * expose MSI without MSI-X. */
    if (!pci_msi_support(dev))
        return -1;

    int vec = msidev_alloc_vector();
    if (vec <= 0) return -1;

    if (msidev_register_handler(vec, handler) != 0 || pci_msi_enable(dev, vec) != 0) {
        msidev_unregister_handler(vec);
        msidev_free_vector(vec);
        return -1;
    }

    pr_info("  %-11s : %02x:%02x.%u on MSI vec 0x%x\n",
            "msidev", (unsigned)dev->bus, (unsigned)dev->dev,
            (unsigned)dev->fn, (unsigned)vec);
    return vec;
}

void msidev_unregister(int vector)
{
    if (vector <= 0) return;
    msidev_unregister_handler(vector);
    msidev_free_vector(vector);
}

void msidev_restore(void)
{
    /* Only MSI-X needs device-side rebuilding: its table lives in MMIO and is
     * cleared by the platform reset, while the MSI message registers sit below
     * 0x100 and travel with the configuration snapshot. */
    msix_restore();
}
