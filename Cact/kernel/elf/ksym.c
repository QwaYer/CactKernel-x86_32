#include "ksym.h"
#include "klib.h"
#include "memory.h"
#include "kernel.h"
#include "net.h"
#include "pci.h"

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
    { "hex_to_ascii",   (uint32_t)hex_to_ascii },
    { "itoa",           (uint32_t)itoa },
    { "kalloc",         (uint32_t)kalloc },
    { "kfree_aligned",  (uint32_t)kfree_aligned },
    { "kfree_heap",     (uint32_t)kfree_heap },
    { "kfree_page",     (uint32_t)kfree_page },
    { "kmalloc",        (uint32_t)kmalloc },
    { "kmalloc_aligned",(uint32_t)kmalloc_aligned },
    { "irq_register_handler", (uint32_t)irq_register_handler },
    { "klog",           (uint32_t)klog },
    { "kprint",         (uint32_t)kprint },
    { "kprint_hex",     (uint32_t)kprint_hex },
    { "memcpy",         (uint32_t)memcpy },
    { "memset",         (uint32_t)memset },
    { "net_receive",         (uint32_t)net_receive },
    { "net_receive_packet",  (uint32_t)net_receive_packet },
    { "net_driver_irq_wake", (uint32_t)net_driver_irq_wake },
    { "net_register_driver",   (uint32_t)net_register_driver },
    { "net_unregister_driver", (uint32_t)net_unregister_driver },
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
