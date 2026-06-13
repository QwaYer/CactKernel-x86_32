#include "kernel.h"
#include "memory.h"
#include "acpi.h"
#include "acpi_hpet.h"
#include "cact_acpi.h"
#include <string.h>

#define HPET_MMIO_VADDR    0xFE002000u

static volatile uint64_t *hpet_regs = NULL;
static uint64_t hpet_fs_per_tick = 0;
static int hpet_available = 0;
static uint64_t hpet_freq = 0;

static inline uint32_t hpet_read32(uint32_t off)
{
    return ((volatile uint32_t *)hpet_regs)[off / 4];
}

static inline void hpet_write32(uint32_t off, uint32_t val)
{
    ((volatile uint32_t *)hpet_regs)[off / 4] = val;
}

uint64_t hpet_read64(uint32_t off)
{
    return (uint64_t)hpet_read32(off + 4) << 32 | hpet_read32(off);
}

void hpet_write64(uint32_t off, uint64_t val)
{
    hpet_write32(off,      (uint32_t)(val & 0xFFFFFFFFu));
    hpet_write32(off + 4,  (uint32_t)(val >> 32));
}

static void hpet_map_mmio(uint32_t phys_base)
{
    uint32_t phys_page = phys_base & ~0xFFF;
    vmm_map(get_current_pd(), HPET_MMIO_VADDR, phys_page,
            PAGE_PRESENT | PAGE_RW);
    hpet_regs = (volatile uint64_t *)(HPET_MMIO_VADDR + (phys_base & 0xFFF));
}

int hpet_init(void)
{
    ACPI_TABLE_HPET *hpet_table = NULL;
    ACPI_STATUS status;

    status = AcpiGetTable("HPET", 1, (ACPI_TABLE_HEADER **)&hpet_table);
    if (ACPI_FAILURE(status) || !hpet_table) {
        klog(LOG_WARN, "HPET: table not found");
        return -1;
    }
    if (hpet_table->Address.SpaceId != ACPI_ADR_SPACE_SYSTEM_MEMORY) {
        klog(LOG_WARN, "HPET: unsupported address space");
        return -1;
    }

    uint32_t hpet_phys = (uint32_t)hpet_table->Address.Address;
    hpet_map_mmio(hpet_phys);

    uint64_t cap_id = hpet_read64(HPET_REG_GCAP_ID);
    hpet_fs_per_tick = cap_id >> 32;
    if (hpet_fs_per_tick == 0) {
        klog(LOG_WARN, "HPET: invalid counter clock period");
        return -1;
    }

    unsigned int num_timers = ((cap_id >> 13) & 0x7) + 1;
    hpet_freq = 1000000000000000ull / hpet_fs_per_tick;

    hpet_write64(HPET_REG_GEN_CONF, 0);
    hpet_write64(HPET_REG_GEN_CONF, HPET_ENABLE_CNF);

    hpet_available = 1;

    {
        char buf[96]; char num[32];
        strcpy(buf, "HPET: base 0x"); hex_to_ascii(hpet_phys, num); strcat(buf, num);
        strcat(buf, ", "); itoa((int)(hpet_freq / 1000000), num); strcat(buf, num);
        strcat(buf, " MHz, "); itoa((int)num_timers, num); strcat(buf, num);
        strcat(buf, " timers");
        klog(LOG_OK, buf);
    }
    return 0;
}

int hpet_start_periodic(unsigned int ioapic_irq, uint64_t period_ticks)
{
    if (!hpet_available) return -1;
    if (period_ticks == 0) return -1;

    uint64_t conf = HPET_TN_TYPE | HPET_TN_INT_ENB | HPET_TN_VAL_CNF;

    if (ioapic_irq < 16)
        conf |= (uint64_t)(ioapic_irq & 0xF) << 12;

    hpet_write64(HPET_REG_TIM0_CONF, conf);
    hpet_write64(HPET_REG_TIM0_COMP, period_ticks);

    uint64_t now = hpet_read64(HPET_REG_MAIN_CNT);
    hpet_write64(HPET_REG_TIM0_COMP, now + period_ticks);

    return 0;
}

bool hpet_is_available(void) { return hpet_available != 0; }
uint64_t hpet_read_counter(void) { return hpet_available ? hpet_read64(HPET_REG_MAIN_CNT) : 0; }
uint64_t hpet_get_usec(void) { return hpet_available ? hpet_read_counter() * 1000000ull / hpet_freq : 0; }
uint64_t hpet_get_freq(void) { return hpet_freq; }
uint32_t hpet_get_ticks(void) { return (uint32_t)(hpet_get_usec() / 10000); }
