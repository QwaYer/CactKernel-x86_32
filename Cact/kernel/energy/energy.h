#ifndef CACT_ENERGY_H
#define CACT_ENERGY_H

#include <stdint.h>

/* C-facing ABI for the Rust energy governor (Cact/kernel/proc/sched/src/energy.rs).
 * Implementation lives in Rust; this header only declares the stable ABI. */

#define ENERGY_MAX_CORES  64
#define ENERGY_MASTER_CPU 0

#define ENERGY_ROLE_NONE   0
#define ENERGY_ROLE_MASTER 1
#define ENERGY_ROLE_WORKER 2

#define ENERGY_CSTATE_C0 0
#define ENERGY_CSTATE_C1 1
#define ENERGY_CSTATE_C3 2
#define ENERGY_CSTATE_C6 3

#define ENERGY_TREND_DOWN   -1
#define ENERGY_TREND_STABLE  0
#define ENERGY_TREND_UP      1

/* Load metrics are scaled to per-mille (1000 = 100%). */
#define ENERGY_PERMILLE 1000u

int energy_init(void);

uint32_t energy_core_count_present(void);
uint32_t energy_core_count_online(void);
uint32_t energy_master_cpu(void);
uint32_t energy_core_lapic_id(uint32_t cpu);
uint32_t energy_core_role(uint32_t cpu);
uint32_t energy_core_cstate(uint32_t cpu);
int      energy_core_is_idle(uint32_t cpu);
int      energy_core_is_present(uint32_t cpu);
int      energy_core_is_online(uint32_t cpu);
int      energy_core_is_master(uint32_t cpu);
int      energy_core_is_worker(uint32_t cpu);

int  energy_core_set_cstate(uint32_t cpu, uint32_t state);
void energy_core_mark_idle(uint32_t cpu);
void energy_core_mark_busy(uint32_t cpu);
int  energy_core_online(uint32_t cpu, uint32_t lapic_id);
void energy_core_offline(uint32_t cpu);

int energy_cstate_init(void);
int energy_cstate_available(uint32_t state);
uint32_t energy_cstate_latency_us(uint32_t state);
uint32_t energy_cstate_min_residency_us(uint32_t state);
uint32_t energy_cstate_wakeup_energy(uint32_t state);
uint32_t energy_cstate_cache_harm_energy(uint32_t state);
int energy_cstate_enter(uint32_t cpu, uint32_t state);
int energy_cstate_idle(uint32_t cpu);

int energy_ipi_init(void);
int energy_ipi_halt_worker(uint32_t cpu);
int energy_ipi_wake_worker(uint32_t cpu);

int energy_monitor_init(void);
uint32_t energy_monitor_load_avg(uint32_t cpu);
uint32_t energy_monitor_queue_length(uint32_t cpu);
uint32_t energy_monitor_cpu_utilization(uint32_t cpu);
int      energy_monitor_trend(uint32_t cpu);
uint32_t energy_monitor_energy_budget(uint32_t cpu);
void     energy_monitor_charge_energy(uint32_t cpu, uint32_t amount);
void     energy_monitor_refill_budget(uint32_t cpu);

uint32_t energy_core_idle_since_tick(uint32_t cpu);

int    energy_decision_init(void);
void   energy_decision_tick(void);
int    energy_decision_should_wake(uint32_t state, uint32_t queue_len,
                                   uint32_t ipl, uint32_t load_permille,
                                   int trend);
int    energy_decision_should_sleep(uint32_t load_permille, uint32_t idle_ms);
uint64_t energy_decision_work_cycles(uint32_t queue_len, uint32_t ipl);
uint64_t energy_decision_benefit(uint32_t queue_len, uint32_t ipl);
uint64_t energy_decision_cost(uint32_t state);

uint32_t energy_mlfq_cap_for_level(uint32_t level);
uint32_t energy_mlfq_runqueue_cap(void);
uint32_t energy_mlfq_idle_target(uint32_t cpu);

int    energy_balance_init(void);
void   energy_balance_tick(void);
uint32_t energy_balance_imbalance_permille(void);
int    energy_balance_migrate(uint32_t src, uint32_t dst, uint32_t pid);

int energy_selftest(void);

#endif
