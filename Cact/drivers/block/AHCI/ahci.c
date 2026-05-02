#include "ahci.h"
#include "pci.h"
#include "pci_enum.h"
#include "pci_driver.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "sync.h"
#include "devfs.h"

// HBA memory-mapped registers — mapped once during init
static hba_mem_t *abar = 0;
// Per-port info: active flag, type, max LBA, model string
static ahci_port_info_t ports_info[AHCI_MAX_PORTS];
// Set after HBA reset + port probe succeeds
static int ahci_ready = 0;
// Serialises all command submissions across ports
static irq_spinlock_t ahci_lock;

// One command header + command table + FIS buffer per port
static hba_cmd_header_t *cmd_headers[AHCI_MAX_PORTS];
static hba_cmd_tbl_t    *cmd_tables[AHCI_MAX_PORTS];
static uint8_t          *fis_bufs[AHCI_MAX_PORTS];

// ~10 us busy-wait with PAUSE hint
static void ahci_udelay(uint32_t us) {
    for (volatile uint32_t i = 0; i < us * 10; i++)
        __asm__ __volatile__("pause");
}

// Decode interface power management and device detection from SSTS
static int ahci_check_type(hba_port_t *port) {
    uint32_t ssts = port->ssts;
    uint8_t ipm = (ssts >> 8) & 0x0F;
    uint8_t det = ssts & 0x0F;

    if (det != HBA_PORT_DET_PRESENT || ipm != HBA_PORT_IPM_ACTIVE)
        return AHCI_DEV_NULL;

    switch (port->sig) {
        case AHCI_SIG_ATAPI: return AHCI_DEV_SATAPI;
        case AHCI_SIG_SEMB:  return AHCI_DEV_SEMB;
        case AHCI_SIG_PM:    return AHCI_DEV_PM;
        default:             return AHCI_DEV_SATA;
    }
}

// Clear ST and FRE, then wait for controller to finish current operation
static void ahci_stop_cmd(hba_port_t *port) {
    port->cmd &= ~HBA_PxCMD_ST;
    port->cmd &= ~HBA_PxCMD_FRE;

    for (int i = 0; i < 1000; i++) {
        if (!(port->cmd & HBA_PxCMD_FR) && !(port->cmd & HBA_PxCMD_CR))
            return;
        ahci_udelay(1000);
    }
    kprint("[AHCI] stop_cmd timeout port\n");
}

// Set FRE and ST after command structures are configured
static void ahci_start_cmd(hba_port_t *port) {
    while (port->cmd & HBA_PxCMD_CR)
        ahci_udelay(100);

    port->cmd |= HBA_PxCMD_FRE;
    port->cmd |= HBA_PxCMD_ST;
}

// Find first free command slot by scanning SACT | CI
static int ahci_find_cmdslot(hba_port_t *port) {
    uint32_t slots = (port->sact | port->ci);
    for (int i = 0; i < AHCI_CMD_SLOTS; i++) {
        if (!(slots & (1u << i)))
            return i;
    }
    kprint("[AHCI] no free cmd slot\n");
    return -1;
}

// Allocate command headers, tables, and FIS buffer for a port, then restart
static void ahci_port_rebase(int portno) {
    hba_port_t *port = &abar->ports[portno];

    ahci_stop_cmd(port);

    // 1 KiB aligned command headers (one per slot)
    cmd_headers[portno] = (hba_cmd_header_t *)kmalloc_aligned(
        sizeof(hba_cmd_header_t) * AHCI_CMD_SLOTS, 1024);
    if (!cmd_headers[portno]) {
        kprint("[AHCI] alloc cmd_headers failed port=");
        char b[16]; itoa(portno, b); kprint(b); kprint("\n");
        return;
    }
    memset((void *)cmd_headers[portno], 0, sizeof(hba_cmd_header_t) * AHCI_CMD_SLOTS);

    port->clb  = (uint32_t)(uintptr_t)cmd_headers[portno];
    port->clbu = 0;

    // 256-byte aligned FIS receive area
    fis_bufs[portno] = (uint8_t *)kmalloc_aligned(256, 256);
    if (!fis_bufs[portno]) {
        kprint("[AHCI] alloc fis_buf failed\n");
        return;
    }
    memset(fis_bufs[portno], 0, 256);

    port->fb  = (uint32_t)(uintptr_t)fis_bufs[portno];
    port->fbu = 0;

    // Command tables — one per slot, 128-byte aligned
    cmd_tables[portno] = (hba_cmd_tbl_t *)kmalloc_aligned(
        sizeof(hba_cmd_tbl_t) * AHCI_CMD_SLOTS, 128);
    if (!cmd_tables[portno]) {
        kprint("[AHCI] alloc cmd_tables failed\n");
        return;
    }
    memset((void *)cmd_tables[portno], 0, sizeof(hba_cmd_tbl_t) * AHCI_CMD_SLOTS);

    // Link each command header to its table, allow up to 8 PRDT entries
    for (int i = 0; i < AHCI_CMD_SLOTS; i++) {
        cmd_headers[portno][i].prdtl = 8;
        cmd_headers[portno][i].ctba  = (uint32_t)(uintptr_t)&cmd_tables[portno][i];
        cmd_headers[portno][i].ctbau = 0;
    }

    port->serr = port->serr;   // clear error register
    port->is   = (uint32_t)-1;  // clear all interrupt status bits
    port->ie   = 0;             // no interrupts — polled I/O

    ahci_start_cmd(port);
}

// Read or write a single 512-byte sector — serialised by ahci_lock
static int ahci_rw_sector(int portno, uint32_t lba, uint8_t *buf, int is_write) {
    if (!ahci_ready || !ports_info[portno].active) return -1;

    hba_port_t *port = &abar->ports[portno];

    irq_spinlock_acquire(&ahci_lock);

    port->is = (uint32_t)-1;  // clear pending interrupts

    int slot = ahci_find_cmdslot(port);
    if (slot < 0) {
        irq_spinlock_release(&ahci_lock);
        return -1;
    }

    // Build command header: one PRDT entry, FIS length in DWORDs
    hba_cmd_header_t *hdr = &cmd_headers[portno][slot];
    hdr->cfis_len = sizeof(fis_reg_h2d_t) / 4;
    hdr->w        = is_write ? 1 : 0;
    hdr->prdtl    = 1;
    hdr->prdbc    = 0;

    // PRDT: single entry covering the user buffer
    hba_cmd_tbl_t *tbl = &cmd_tables[portno][slot];
    memset((void *)tbl, 0, sizeof(hba_cmd_tbl_t));

    tbl->prdt[0].dba  = (uint32_t)(uintptr_t)buf;
    tbl->prdt[0].dbau = 0;
    tbl->prdt[0].dbc  = AHCI_SECTOR_SIZE - 1;
    tbl->prdt[0].dbc |= (1u << 31);   // interrupt on completion

    // FIS: register Host-to-Device, LBA28, 1 sector
    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)tbl->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));

    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c        = 1;                    // command register write
    fis->command  = is_write ? ATA_CMD_WRITE_DMA_EX : ATA_CMD_READ_DMA_EX;
    fis->device   = 1 << 6;               // LBA mode

    fis->lba0 = (uint8_t)(lba);
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = 0;
    fis->lba5 = 0;

    fis->count = 1;  // 1 sector

    // Wait for port to leave BSY/DRQ state before issuing the command
    for (int i = 0; i < 1000; i++) {
        if (!(port->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ))) break;
        ahci_udelay(1000);
    }
    if (port->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)) {
        irq_spinlock_release(&ahci_lock);
        return -1;
    }

    // Issue command and poll for completion (or task file error)
    port->ci = (1u << slot);

    for (int i = 0; i < 2000000; i++) {
        if (!(port->ci & (1u << slot)))
            break;                                    // success
        if (port->is & HBA_PxIS_TFES) {
            kprint("[AHCI] task file error lba=");
            char b[16]; hex_to_ascii(lba, b); kprint(b); kprint("\n");
            irq_spinlock_release(&ahci_lock);
            return -1;
        }
    }

    if (port->ci & (1u << slot)) {
        kprint("[AHCI] cmd timeout lba=");
        char b[16]; hex_to_ascii(lba, b); kprint(b); kprint("\n");
        irq_spinlock_release(&ahci_lock);
        return -1;
    }

    // Sanity check after completion
    if (port->is & HBA_PxIS_TFES) {
        kprint("[AHCI] task file error after completion\n");
        irq_spinlock_release(&ahci_lock);
        return -1;
    }

    irq_spinlock_release(&ahci_lock);
    return 0;
}

// Send IDENTIFY DEVICE, extract max LBA and model string
static int ahci_identify(int portno) {
    hba_port_t *port = &abar->ports[portno];
    uint8_t *id_buf = (uint8_t *)kmalloc_aligned(512, 512);
    if (!id_buf) return -1;
    memset(id_buf, 0, 512);

    port->is = (uint32_t)-1;

    int slot = ahci_find_cmdslot(port);
    if (slot < 0) { kfree_aligned(id_buf); return -1; }

    hba_cmd_header_t *hdr = &cmd_headers[portno][slot];
    hdr->cfis_len = sizeof(fis_reg_h2d_t) / 4;
    hdr->w        = 0;    // read
    hdr->prdtl    = 1;
    hdr->prdbc    = 0;

    hba_cmd_tbl_t *tbl = &cmd_tables[portno][slot];
    memset((void *)tbl, 0, sizeof(hba_cmd_tbl_t));

    tbl->prdt[0].dba  = (uint32_t)(uintptr_t)id_buf;
    tbl->prdt[0].dbau = 0;
    tbl->prdt[0].dbc  = 511;         // 512 bytes - 1
    tbl->prdt[0].dbc |= (1u << 31);

    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)tbl->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c        = 1;
    fis->command  = ATA_CMD_IDENTIFY;
    fis->device   = 0;

    for (int i = 0; i < 1000; i++) {
        if (!(port->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ))) break;
        ahci_udelay(1000);
    }

    port->ci = (1u << slot);

    for (int i = 0; i < 2000000; i++) {
        if (!(port->ci & (1u << slot))) break;
        if (port->is & HBA_PxIS_TFES) {
            kprint("[AHCI] identify TFE port=");
            char b[16]; itoa(portno, b); kprint(b); kprint("\n");
            kfree_aligned(id_buf);
            return -1;
        }
    }

    if (port->ci & (1u << slot)) {
        kprint("[AHCI] identify timeout port=");
        char b[16]; itoa(portno, b); kprint(b); kprint("\n");
        kfree_aligned(id_buf);
        return -1;
    }

    // Words 60-61: LBA28 capacity
    uint16_t *id16 = (uint16_t *)id_buf;
    uint32_t lba28 = (uint32_t)id16[60] | ((uint32_t)id16[61] << 16);
    ports_info[portno].max_lba = lba28;

    // Words 27-46: model string (40 bytes, byte-swapped)
    for (int i = 0; i < 20; i++) {
        uint16_t w = id16[27 + i];
        ports_info[portno].model[i * 2]     = (char)(w >> 8);
        ports_info[portno].model[i * 2 + 1] = (char)(w & 0xFF);
    }
    ports_info[portno].model[40] = '\0';

    // Trim trailing spaces
    for (int i = 39; i >= 0 && ports_info[portno].model[i] == ' '; i--)
        ports_info[portno].model[i] = '\0';

    kfree_aligned(id_buf);
    return 0;
}

// Iterate implemented ports, rebase and identify SATA devices
static void ahci_probe_ports(void) {
    uint32_t pi = abar->pi;  // Port Implemented bitmask

    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        if (!(pi & (1u << i))) continue;

        int dt = ahci_check_type(&abar->ports[i]);
        if (dt == AHCI_DEV_SATA) {
            ports_info[i].active   = 1;
            ports_info[i].type     = AHCI_DEV_SATA;
            ports_info[i].port_num = (uint8_t)i;

            ahci_port_rebase(i);
            ahci_identify(i);
        } else if (dt == AHCI_DEV_SATAPI) {
            // ATAPI not handled — silently ignored
        }
    }
}

// Map MMIO range with cache disabled, reset HBA, probe ports
static int ahci_init_controller(uint32_t mmio) {
    uint32_t bar_size = 0x2000;   // 8 KiB typical HBA BAR

    // Map with PCD|PWT so writes reach the controller, not just the CPU cache
    for (uint32_t off = 0; off < bar_size; off += 0x1000)
        vmm_map(get_current_pd(), mmio + off, mmio + off,
                PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);

    abar = (hba_mem_t *)(uintptr_t)mmio;

    // HBA reset sequence: AE=1, then HR=1, wait, HR=0, wait, AE=1
    abar->ghc |= (1u << 31);     // AE — enable

    abar->ghc |= (1u << 0);      // HR — start reset
    ahci_udelay(100000);
    abar->ghc &= ~(1u << 0);     // clear HR

    for (int i = 0; i < 1000; i++) {
        if (!(abar->ghc & (1u << 0))) break;
        ahci_udelay(1000);
    }
    if (abar->ghc & (1u << 0)) {
        kprint("[AHCI] HBA reset timeout\n");
        return -1;
    }

    abar->ghc |= (1u << 31);     // re-enable AE after reset

    abar->is = (uint32_t)-1;     // clear all pending interrupts

    memset(ports_info, 0, sizeof(ports_info));

    ahci_probe_ports();

    ahci_ready = 1;
    return 0;
}

// PCI probe callback: locate MMIO BAR, enable bus-mastering, init controller
static int ahci_pci_probe(pci_device_t *pdev) {
    if (pdev->prog_if != 0x01)   // only AHCI (not IDE-emulation)
        return -1;

    uint32_t mmio = 0;
    for (int i = 0; i < 6; i++) {
        if (!pdev->bars[i].is_io && pdev->bars[i].base) {
            mmio = pdev->bars[i].base; break;
        }
    }
    if (!mmio)
        mmio = pci_read32(pdev->bus, pdev->dev, pdev->fn, 0x24) & ~0xFu;
    if (!mmio) {
        kprint("[AHCI] No MMIO BAR\n");
        return -1;
    }

    // Enable bus mastering + memory space
    uint32_t cmd = pci_read32(pdev->bus, pdev->dev, pdev->fn, 0x04);
    pci_write32(pdev->bus, pdev->dev, pdev->fn, 0x04, cmd | 0x06);

    // Disable INTx emulation (bit 10)
    cmd = pci_read32(pdev->bus, pdev->dev, pdev->fn, 0x04);
    pci_write32(pdev->bus, pdev->dev, pdev->fn, 0x04, cmd & ~(1u << 10));

    return ahci_init_controller(mmio);
}

// PCI driver descriptor — class 01/06 (SATA AHCI)
static pci_driver_t ahci_pci_driver = {
    .name       = "ahci_hba",
    .vendor_id  = PCI_ANY_ID,
    .device_id  = PCI_ANY_ID,
    .class_code = 0x01,
    .subclass   = 0x06,
    .probe      = ahci_pci_probe,
};

// devfs read: read sectors from first active port by offset
static int _ahci_devfs_read(void *p, uint32_t off, uint32_t size, char *buf) {
    (void)p;
    uint8_t sector_buf[512] __attribute__((aligned(512)));
    int port = ahci_first_port();
    if (port < 0) return -1;

    uint32_t lba = off / 512, written = 0;
    while (written < size) {
        if (ahci_rw_sector(port, lba, sector_buf, 0) < 0) return -1;
        uint32_t c = 512;
        if (c > size - written) c = size - written;
        memcpy(buf + written, sector_buf, c);
        written += c;
        lba++;
    }
    return (int)written;
}

// devfs write: read-modify-write for partial-sector writes
static int _ahci_devfs_write(void *p, uint32_t off, uint32_t size, char *buf) {
    (void)p;
    uint8_t sector_buf[512] __attribute__((aligned(512)));
    int port = ahci_first_port();
    if (port < 0) return -1;

    uint32_t lba = off / 512, written = 0;
    while (written < size) {
        if (ahci_rw_sector(port, lba, sector_buf, 0) < 0) return -1;
        uint32_t c = 512;
        if (c > size - written) c = size - written;
        memcpy(sector_buf, buf + written, c);
        if (ahci_rw_sector(port, lba, sector_buf, 1) < 0) return -1;
        written += c;
        lba++;
    }
    return (int)written;
}

// devfs status: returns device info string (model, type)
static int _ahci_devfs_status(void *p, char *buf, uint32_t size) {
    (void)p;
    int port = ahci_first_port();
    if (port < 0) {
        const char *s = "device: sda\ntype: AHCI (no ports)\n";
        uint32_t n = 0;
        while (s[n] && n < size - 1) { buf[n] = s[n]; n++; }
        buf[n] = '\0';
        return (int)n;
    }

    char tmp[256];
    int pos = 0;
    const char *h1 = "device: sda\ntype: AHCI/SATA\nmodel: ";
    while (h1[pos] && (uint32_t)pos < size - 1) { buf[pos] = h1[pos]; pos++; }
    for (int i = 0; ports_info[port].model[i] && (uint32_t)pos < size - 1; i++)
        buf[pos++] = ports_info[port].model[i];
    buf[pos++] = '\n';
    buf[pos] = '\0';
    (void)tmp;
    return pos;
}

static devfs_driver_t drv_ahci = {
    .read   = _ahci_devfs_read,
    .write  = _ahci_devfs_write,
    .status = _ahci_devfs_status,
};

// Public API: read a single sector from the given port
void ahci_read_sector(uint32_t port, uint32_t lba, uint8_t *buf) {
    if (ahci_rw_sector((int)port, lba, buf, 0) < 0)
        kprint("[AHCI] read_sector failed\n");
}

// Public API: write a single sector to the given port
void ahci_write_sector(uint32_t port, uint32_t lba, uint8_t *buf) {
    if (ahci_rw_sector((int)port, lba, buf, 1) < 0)
        kprint("[AHCI] write_sector failed\n");
}

// Return number of active SATA ports
int ahci_port_count(void) {
    int cnt = 0;
    for (int i = 0; i < AHCI_MAX_PORTS; i++)
        if (ports_info[i].active) cnt++;
    return cnt;
}

// Return index of first active port, or -1 if none found
int ahci_first_port(void) {
    for (int i = 0; i < AHCI_MAX_PORTS; i++)
        if (ports_info[i].active) return i;
    return -1;
}

// Initialise AHCI: register PCI driver, scan bus, rebase ports, register devfs
void ahci_init(void) {
    kprint("[AHCI] initializing lock and registering PCI driver\n");
    irq_spinlock_init(&ahci_lock);

    pci_register_driver(&ahci_pci_driver);

    kprint("[AHCI] searching PCI for class=01/06 (SATA controller)\n");
    pci_device_t *d = pci_find_by_class(0x01, 0x06);
    while (d) {
        if (d->prog_if == 0x01) {
            kprint("[AHCI] found HBA at bus="); kprint_hex(d->bus);
            kprint(" dev="); kprint_hex(d->dev); kprint(" fn="); kprint_hex(d->fn);
            kprint("\n[AHCI] probing controller (MMIO BAR, bus master, HBA reset)\n");
            ahci_pci_probe(d);
            break;
        }
        d = d->next;
        while (d && !(d->class_code == 0x01 && d->subclass == 0x06))
            d = d->next;
    }

    if (!d) {
        klog(LOG_WARN, "AHCI: no SATA controller on PCI bus");
        return;
    }

    int port = ahci_first_port();
    if (ahci_ready && port >= 0) {
        char tmp[16]; itoa(port, tmp);
        kprint("[AHCI] active SATA port="); kprint(tmp);
        kprint(" model="); kprint(ports_info[port].model); kprint("\n");
        kprint("[AHCI] registering devfs node 'sda'\n");
        devfs_register("sda", DEVFS_F_BLOCK, &drv_ahci, 0);
        klog(LOG_OK, "AHCI ready — /dev/sda registered");
    } else {
        kprint("[AHCI] no SATA ports found\n");
        klog(LOG_WARN, "AHCI: no active SATA ports");
    }
}

// Return max LBA28 address for a port, or 0 if invalid/inactive
uint32_t ahci_get_max_lba(int port) {
    if (port < 0 || port >= AHCI_MAX_PORTS || !ports_info[port].active) return 0;
    return ports_info[port].max_lba;
}