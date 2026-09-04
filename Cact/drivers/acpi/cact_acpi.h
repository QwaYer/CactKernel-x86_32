#ifndef CACT_ACPI_H
#define CACT_ACPI_H

#include <stdint.h>

#define ACPI_TEMP_MAP_BASE   0xC01C0000u
#define ACPI_TEMP_MAP_PAGES  256
#define ACPI_TEMP_MAP_SIZE   (ACPI_TEMP_MAP_PAGES * 4096u)

#define RSDP_SIG_LEN   8
#define RSDP_SIG       "RSD PTR "
#define RSDP_SCAN_START  0x000E0000u
#define RSDP_SCAN_END    0x000FFFFFu
#define RSDP_SCAN_STEP   16u

#define EBDA_SEGMENT   0x040E
#define EBDA_OFFSET    0x0000

struct rsdp_descriptor {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
} __attribute__((packed));

struct rsdp_descriptor_v2 {
    struct rsdp_descriptor base;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    char     creator_id[4];
    uint32_t creator_revision;
} __attribute__((packed));

int  acpi_init(void);
int  acpi_available(void);
void acpi_osc_pcie_init(void);
void acpi_power_off(void);
void acpi_reboot(void);

/* Shared OSL helpers (osl.c): temporary physical->virtual mapping window and
 * a busy-loop microsecond delay. */
void *acpi_temp_map(uint32_t phys, uint32_t size);
void  acpi_temp_unmap(void *virt, uint32_t size);
void  osl_udelay(uint32_t us);

#endif
