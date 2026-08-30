#include "ksym.h"
#include "klib.h"
#include "memory.h"
#include "kernel.h"
#include "net.h"
#include "pci.h"
#include "sync.h"
#include "devfs.h"
#include "blkdev.h"
#include "pagecache.h"
#include "msi.h"

typedef struct {
    const char* name;
    uint32_t    addr;
} ksym_entry_t;

/*
 * Static export list — extend as driver modules need more kernel entry points.
 * Keep sorted by name if you switch to binary search; linear scan is fine
 * for a small table.
 */
static const ksym_entry_t ksym_table[] = {
    { "compare_string", (uint32_t)compare_string },
    { "copy_string",    (uint32_t)copy_string },
    { "hex_to_ascii",   (uint32_t)hex_to_ascii },
    { "itoa",           (uint32_t)itoa },
    { "kalloc",         (uint32_t)kalloc },
    { "kfree_aligned",  (uint32_t)kfree_aligned },
    { "kfree_heap",     (uint32_t)kfree_heap },
    { "kfree_page",     (uint32_t)kfree_page },
    { "kmalloc",        (uint32_t)kmalloc },
    { "kmalloc_aligned",(uint32_t)kmalloc_aligned },
    { "memory_copy",    (uint32_t)memory_copy },
    { "memory_set",     (uint32_t)memory_set },
    { "irq_spinlock_init",     (uint32_t)irq_spinlock_init },
    { "irq_spinlock_acquire",  (uint32_t)irq_spinlock_acquire },
    { "irq_spinlock_release",  (uint32_t)irq_spinlock_release },
    { "sema_init",             (uint32_t)sema_init },
    { "sema_down",             (uint32_t)sema_down },
    { "sema_up",               (uint32_t)sema_up },
    { "mutex_init",            (uint32_t)mutex_init },
    { "mutex_lock",            (uint32_t)mutex_lock },
    { "mutex_unlock",          (uint32_t)mutex_unlock },
    { "devfs_register",        (uint32_t)devfs_register },
    { "devfs_unregister",      (uint32_t)devfs_unregister },
    { "blkdev_register",       (uint32_t)blkdev_register },
    { "blkdev_unregister",     (uint32_t)blkdev_unregister },
    { "blkdev_read_sector",    (uint32_t)blkdev_read_sector },
    { "blkdev_write_sector",   (uint32_t)blkdev_write_sector },
    { "pc_get_page",           (uint32_t)pc_get_page },
    { "pc_put_page",           (uint32_t)pc_put_page },
    { "pc_mark_dirty",         (uint32_t)pc_mark_dirty },
    { "pc_flush_dev",          (uint32_t)pc_flush_dev },
    { "klog",           (uint32_t)klog },
    { "kprint",         (uint32_t)kprint },
    { "kprint_color",   (uint32_t)kprint_color },
    { "kprint_hex",     (uint32_t)kprint_hex },
    { "memcpy",         (uint32_t)memcpy },
    { "memset",         (uint32_t)memset },
    { "net_receive",         (uint32_t)net_receive },
    { "net_receive_packet",  (uint32_t)net_receive_packet },
    { "net_driver_irq_wake", (uint32_t)net_driver_irq_wake },
    { "active_nic",          (uint32_t)&active_nic },
    { "net_register_driver",   (uint32_t)net_register_driver },
    { "net_unregister_driver", (uint32_t)net_unregister_driver },
    { "msix_alloc_vector",    (uint32_t)msix_alloc_vector },
    { "msix_free_vector",     (uint32_t)msix_free_vector },
    { "msix_register_handler", (uint32_t)msix_register_handler },
    { "msix_unregister_handler", (uint32_t)msix_unregister_handler },
    { "pci_msix_support",     (uint32_t)pci_msix_support },
    { "pci_msix_table_map",   (uint32_t)pci_msix_table_map },
    { "pci_msix_pba_map",     (uint32_t)pci_msix_pba_map },
    { "pci_msix_enable",      (uint32_t)pci_msix_enable },
    { "pci_enable_bus_master", (uint32_t)pci_enable_bus_master },
    { "pci_read32",            (uint32_t)pci_read32 },
    { "pci_read_config_long",  (uint32_t)pci_read_config_long },
    { "pci_write32",           (uint32_t)pci_write32 },
    { "pci_write_config_long", (uint32_t)pci_write_config_long },
    { "port_byte_in",   (uint32_t)port_byte_in },
    { "port_byte_out",  (uint32_t)port_byte_out },
    { "port_long_in",   (uint32_t)port_long_in },
    { "port_long_out",  (uint32_t)port_long_out },
    { "port_word_in",   (uint32_t)port_word_in },
    { "port_word_out",  (uint32_t)port_word_out },
    { "strcmp",         (uint32_t)strcmp },
    { "strlen",         (uint32_t)strlen },
    { "strncmp",        (uint32_t)strncmp },
    { "strncpy",        (uint32_t)strncpy },
    { "streq",          (uint32_t)streq },
    { "strlcpy",        (uint32_t)strlcpy },
    { "skb_alloc",      (uint32_t)skb_alloc },
    { "skb_data",       (uint32_t)skb_data },
    { "skb_free",       (uint32_t)skb_free },
    { "skb_len",        (uint32_t)skb_len },
    { "skb_push",       (uint32_t)skb_push },
    { "skb_put",        (uint32_t)skb_put },
    { "vmm_get_phys",   (uint32_t)vmm_get_phys },
    { "vmm_map",        (uint32_t)vmm_map },
};

uint32_t ksym_resolve(const char* name) {
    if (!name) return 0;
    for (unsigned i = 0; i < sizeof(ksym_table) / sizeof(ksym_table[0]); i++) {
        if (streq((char*)name, (char*)ksym_table[i].name))
            return ksym_table[i].addr;
    }
    return 0;
}
