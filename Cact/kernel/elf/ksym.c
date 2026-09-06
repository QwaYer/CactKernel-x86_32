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
    { "kmalloc_aligned", (uint32_t)kmalloc_aligned },
    { "compare_string",  (uint32_t)compare_string },
    { "copy_string",     (uint32_t)copy_string },
    { "memory_copy",     (uint32_t)memory_copy },
    { "memory_set",      (uint32_t)memory_set },
    { "irq_spinlock_init",    (uint32_t)irq_spinlock_init },
    { "irq_spinlock_acquire", (uint32_t)irq_spinlock_acquire },
    { "irq_spinlock_release", (uint32_t)irq_spinlock_release },
    { "free_page",      (uint32_t)free_page },
    { "kfree",          (uint32_t)kfree },
    { "kmalloc",        (uint32_t)kmalloc },
    { "snprintf",       (uint32_t)snprintf },
    { "memcpy",         (uint32_t)memcpy },
    { "memset",         (uint32_t)memset },
    { "sema_init",      (uint32_t)sema_init },
    { "down",           (uint32_t)down },
    { "up",             (uint32_t)up },
    { "mutex_init",     (uint32_t)mutex_init },
    { "mutex_lock",     (uint32_t)mutex_lock },
    { "mutex_unlock",   (uint32_t)mutex_unlock },
    { "spin_lock_init", (uint32_t)spin_lock_init },
    { "spin_lock",      (uint32_t)spin_lock },
    { "spin_unlock",    (uint32_t)spin_unlock },
    { "spin_lock_irqsave",    (uint32_t)spin_lock_irqsave },
    { "spin_unlock_irqrestore", (uint32_t)spin_unlock_irqrestore },
    { "register_chrdev",      (uint32_t)register_chrdev },
    { "unregister_chrdev",    (uint32_t)unregister_chrdev },
    { "register_blkdev",       (uint32_t)register_blkdev },
    { "unregister_blkdev",     (uint32_t)unregister_blkdev },
    { "blkdev_read_sector",    (uint32_t)blkdev_read_sector },
    { "blkdev_write_sector",   (uint32_t)blkdev_write_sector },
    { "blkdev_read",           (uint32_t)blkdev_read },
    { "blkdev_write",          (uint32_t)blkdev_write },
    { "blkdev_by_id",          (uint32_t)blkdev_by_id },
    { "pc_get_page",           (uint32_t)pc_get_page },
    { "pc_put_page",           (uint32_t)pc_put_page },
    { "pc_mark_dirty",         (uint32_t)pc_mark_dirty },
    { "pc_flush_dev",          (uint32_t)pc_flush_dev },
    { "printk",         (uint32_t)printk },
    { "printk_color",   (uint32_t)printk_color },
    { "printk_hex",     (uint32_t)printk_hex },
    { "netif_rx",         (uint32_t)netif_rx },
    { "net_receive_packet",  (uint32_t)net_receive_packet },
    { "net_driver_irq_wake", (uint32_t)net_driver_irq_wake },
    { "active_nic",          (uint32_t)&active_nic },
    { "register_netdev",   (uint32_t)register_netdev },
    { "unregister_netdev", (uint32_t)unregister_netdev },
    { "msix_alloc_vector",    (uint32_t)msix_alloc_vector },
    { "msix_free_vector",     (uint32_t)msix_free_vector },
    { "msix_register_handler", (uint32_t)msix_register_handler },
    { "msix_unregister_handler", (uint32_t)msix_unregister_handler },
    { "pci_msix_support",     (uint32_t)pci_msix_support },
    { "pci_msix_table_map",   (uint32_t)pci_msix_table_map },
    { "pci_msix_pba_map",     (uint32_t)pci_msix_pba_map },
    { "pci_msix_enable",      (uint32_t)pci_msix_enable },
    { "pci_set_master", (uint32_t)pci_set_master },
    { "pci_read_config_dword",            (uint32_t)pci_read_config_dword },
    { "pci_read_config_long",  (uint32_t)pci_read_config_long },
    { "pci_write_config_dword",           (uint32_t)pci_write_config_dword },
    { "pci_write_config_long", (uint32_t)pci_write_config_long },
    { "inb",   (uint32_t)inb },
    { "outb",  (uint32_t)outb },
    { "inl",   (uint32_t)inl },
    { "outl",  (uint32_t)outl },
    { "inw",   (uint32_t)inw },
    { "outw",  (uint32_t)outw },
    { "strcmp",         (uint32_t)strcmp },
    { "strlen",         (uint32_t)strlen },
    { "strncmp",        (uint32_t)strncmp },
    { "strncpy",        (uint32_t)strncpy },
    { "streq",          (uint32_t)streq },
    { "strlcpy",        (uint32_t)strlcpy },
    { "skb_alloc",      (uint32_t)skb_alloc },
    { "skb_data",       (uint32_t)skb_data },
    { "kfree_skb",       (uint32_t)kfree_skb },
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
