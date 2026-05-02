#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>

// Controller limits
#define AHCI_MAX_PORTS       32
#define AHCI_CMD_SLOTS       32
#define AHCI_SECTOR_SIZE     512

// Port signatures in PxSIG after device detection
#define AHCI_SIG_ATA         0x00000101
#define AHCI_SIG_ATAPI       0xEB140101
#define AHCI_SIG_SEMB        0xC33C0101
#define AHCI_SIG_PM          0x96690101

// ahci_check_type return values
#define AHCI_DEV_NULL        0
#define AHCI_DEV_SATA        1
#define AHCI_DEV_SATAPI      2
#define AHCI_DEV_SEMB        3
#define AHCI_DEV_PM          4

// SSTS fields
#define HBA_PORT_IPM_ACTIVE  1
#define HBA_PORT_DET_PRESENT 3

// PxCMD bits
#define HBA_PxCMD_ST    0x0001   // start
#define HBA_PxCMD_FRE   0x0010   // FIS receive enable
#define HBA_PxCMD_FR    0x4000   // FIS receive running
#define HBA_PxCMD_CR    0x8000   // command list running

// PxIS — interrupt status
#define HBA_PxIS_TFES   (1u << 30)  // task file error

// ATA commands
#define ATA_CMD_READ_DMA_EX  0x25
#define ATA_CMD_WRITE_DMA_EX 0x35
#define ATA_CMD_IDENTIFY     0xEC

// ATA status bits
#define ATA_DEV_BUSY  0x80
#define ATA_DEV_DRQ   0x08

// FIS type codes
#define FIS_TYPE_REG_H2D     0x27
#define FIS_TYPE_REG_D2H     0x34
#define FIS_TYPE_DMA_ACT     0x39
#define FIS_TYPE_DMA_SETUP   0x41
#define FIS_TYPE_DATA        0x46
#define FIS_TYPE_BIST        0x58
#define FIS_TYPE_PIO_SETUP   0x5F
#define FIS_TYPE_DEV_BITS    0xA1

// HBA port registers (volatile — MMIO)
typedef volatile struct {
    uint32_t clb;            // command list base address
    uint32_t clbu;           // command list base upper
    uint32_t fb;             // FIS base address
    uint32_t fbu;            // FIS base upper
    uint32_t is;             // interrupt status
    uint32_t ie;             // interrupt enable
    uint32_t cmd;            // command (ST, FRE, CR, FR)
    uint32_t rsv0;
    uint32_t tfd;            // task file data (BSY, DRQ, ERR)
    uint32_t sig;            // device signature
    uint32_t ssts;           // serial ATA status (DET, IPM)
    uint32_t sctl;           // serial ATA control
    uint32_t serr;           // serial ATA error
    uint32_t sact;           // active slots
    uint32_t ci;             // command issue
    uint32_t sntf;           // SATA notification
    uint32_t fbs;            // FIS-based switching
    uint32_t rsv1[11];
    uint32_t vendor[4];
} hba_port_t;

// HBA memory-mapped registers (ABAR)
typedef volatile struct {
    uint32_t cap;            // host capabilities
    uint32_t ghc;            // global host control (AE, HR)
    uint32_t is;             // interrupt status
    uint32_t pi;             // ports implemented
    uint32_t vs;             // version
    uint32_t ccc_ctl;        // command completion coalescing control
    uint32_t ccc_pts;        // CCC ports
    uint32_t em_loc;         // enclosure management location
    uint32_t em_ctl;         // enclosure management control
    uint32_t cap2;           // host capabilities extended
    uint32_t bohc;           // BIOS/OS handoff control
    uint8_t  rsv[0xA0 - 0x2C];
    uint8_t  vendor[0x100 - 0xA0];
    hba_port_t ports[32];
} hba_mem_t;

// Command header (one per slot)
typedef struct {
    uint8_t  cfis_len : 5;   // FIS length in DWORDs
    uint8_t  a        : 1;   // ATAPI
    uint8_t  w        : 1;   // write (1) / read (0)
    uint8_t  p        : 1;   // prefetchable
    uint8_t  prdtl_lo;       // PRDT length (low byte)
    uint16_t prdtl;          // PRDT length (full)
    uint32_t prdbc;          // bytes transferred
    uint32_t ctba;           // command table base address
    uint32_t ctbau;          // command table base upper
    uint32_t rsv[4];
} __attribute__((packed)) hba_cmd_header_t;

// Physical region descriptor table entry
typedef struct {
    uint32_t dba;            // data base address
    uint32_t dbau;           // data base upper
    uint32_t rsv;
    uint32_t dbc;            // byte count (bit 31 = interrupt on completion)
} __attribute__((packed)) hba_prdt_entry_t;

// Command table (one per slot)
typedef struct {
    uint8_t cfis[64];        // command FIS
    uint8_t acmd[16];        // ATAPI command
    uint8_t rsv[48];
    hba_prdt_entry_t prdt[8]; // up to 8 PRDT entries
} __attribute__((packed)) hba_cmd_tbl_t;

// Register FIS — Host to Device (28-bit LBA)
typedef struct {
    uint8_t  fis_type;
    uint8_t  pm_port : 4;
    uint8_t  rsv0    : 3;
    uint8_t  c       : 1;    // command register write
    uint8_t  command;
    uint8_t  featurel;
    uint8_t  lba0;
    uint8_t  lba1;
    uint8_t  lba2;
    uint8_t  device;
    uint8_t  lba3;
    uint8_t  lba4;
    uint8_t  lba5;
    uint8_t  featureh;
    uint16_t count;
    uint8_t  icc;
    uint8_t  control;
    uint32_t rsv1;
} __attribute__((packed)) fis_reg_h2d_t;

// Per-port metadata (populated during probe)
typedef struct {
    uint8_t  active;
    uint8_t  type;
    uint8_t  port_num;
    uint32_t max_lba;
    char     model[41];
} ahci_port_info_t;

// Public API
void ahci_init(void);
void ahci_read_sector (uint32_t port, uint32_t lba, uint8_t *buf);
void ahci_write_sector(uint32_t port, uint32_t lba, uint8_t *buf);
int  ahci_port_count  (void);
int  ahci_first_port  (void);
uint32_t ahci_get_max_lba(int port);

#endif