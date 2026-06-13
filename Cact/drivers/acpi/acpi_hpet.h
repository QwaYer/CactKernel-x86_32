#ifndef CACT_HPET_H
#define CACT_HPET_H

#include <stdint.h>
#include <stdbool.h>

#define HPET_REG_GCAP_ID       0x000
#define HPET_REG_GEN_CONF      0x010
#define HPET_REG_GEN_INTR_STA  0x020
#define HPET_REG_MAIN_CNT      0x0F0
#define HPET_REG_TIM0_CONF     0x100
#define HPET_REG_TIM0_COMP     0x108

#define HPET_ENABLE_CNF        0x001
#define HPET_LEG_RT_CNF        0x002

#define HPET_TN_TYPE           (1u << 1)
#define HPET_TN_INT_ENB        (1u << 2)
#define HPET_TN_VAL_CNF        (1u << 5)

int  hpet_init(void);
int  hpet_start_periodic(unsigned int ioapic_irq, uint64_t period_ticks);
bool hpet_is_available(void);
uint64_t hpet_read_counter(void);
uint64_t hpet_get_usec(void);
uint64_t hpet_get_freq(void);
uint32_t hpet_get_ticks(void);
void hpet_write64(uint32_t off, uint64_t val);
uint64_t hpet_read64(uint32_t off);

#endif
