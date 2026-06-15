#ifndef PCIE_H
#define PCIE_H

#include <stdint.h>
#include <stdbool.h>

#define PCIE_CAP_ID             0x10

#define PCIE_EXT_CAP_ID_AER     0x0001
#define PCIE_EXT_CAP_ID_VC      0x0002
#define PCIE_EXT_CAP_ID_SERIAL  0x0003
#define PCIE_EXT_CAP_ID_PWR_BUDGET 0x0004
#define PCIE_EXT_CAP_ID_RCLINK  0x0005
#define PCIE_EXT_CAP_ID_RCLINK_CTRL  0x0006
#define PCIE_EXT_CAP_ID_SLOT_POWER  0x0007
#define PCIE_EXT_CAP_ID_ACS     0x000D
#define PCIE_EXT_CAP_ID_PASID   0x001B
#define PCIE_EXT_CAP_ID_DPA     0x001F

#define PCIE_CONFIG_SPACE_SIZE  4096

#define PCIE_ECAM_VADDR         0xC0000000u
#define PCIE_MAX_SEGMENTS       4

#define PCIE_TYPE_ENDPOINT          0x0
#define PCIE_TYPE_LEGACY_ENDPOINT   0x1
#define PCIE_TYPE_ROOT_PORT         0x4
#define PCIE_TYPE_UPSTREAM_PORT     0x5
#define PCIE_TYPE_DOWNSTREAM_PORT   0x6
#define PCIE_TYPE_PCI_BRIDGE        0x7

#define PCIE_CAP_ROLE_BASED_ERR     0x01
#define PCIE_CAP_SLOT_IMPLEMENTED   0x02
#define PCIE_CAP_MSI_MASK           0x10
#define PCIE_CAP_EXTENDED_TAG       0x20

bool pcie_init(void);
bool pcie_is_available(void);

uint32_t pcie_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg);
void     pcie_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg, uint32_t val);
uint8_t  pcie_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg);
uint16_t pcie_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg);
void     pcie_write8(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg, uint8_t val);
void     pcie_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg, uint16_t val);

int      pcie_find_cap(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t cap_id);
int      pcie_get_type(uint8_t bus, uint8_t dev, uint8_t fn);
uint16_t pcie_read_ext_cap(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t cap_id, uint16_t offset);
void     pcie_dump_all(void);

#endif
