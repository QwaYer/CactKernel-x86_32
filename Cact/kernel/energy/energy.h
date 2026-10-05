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

/* CPU topology (P1.2): decoded once at boot by sched/src/cpu_topo.rs from CPUID
 * leaf 0xB/0x1F (HTT fallback: leaf 1 + leaf 4).  Per-logical-CPU package/core
 * ids plus the per-package counts that /proc/cpuinfo prints. */
void     cpu_topo_init(void);
int      cpu_topo_valid(void);
uint32_t cpu_topo_package(uint32_t cpu);
uint32_t cpu_topo_core(uint32_t cpu);
uint32_t cpu_topo_siblings(uint32_t cpu);
uint32_t cpu_topo_cpu_cores(uint32_t cpu);
uint32_t cpu_topo_threads_per_core(void);
uint32_t cpu_topo_packages(void);

/* Sibling-aware placement self-test (P1.2): 0 when the policy lands the second
 * thread on a physical core other than the master's when SMT is present. */
int      placement_selftest(void);

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

/* Physical core offlining/onlining (the slow tier).  Must run from task
 * context (the master's idle loop): the online sequence (INIT-SIPI-SIPI +
 * waits) must not block the timer ISR.
 *
 * On by default.  The re-online path re-runs `smp_ap_entry` through the boot
 * trampoline, so it depends on per-CPU state a first bring-up does not need
 * (the TSS descriptor's busy bit) — see the notes in sched/src/smp.rs and
 * sched/src/decision.rs for what that cost the last time it broke.  Kill
 * switch: energy_core_offline_enable(0). */
void energy_core_manage(void);
void energy_core_offline_enable(int on);

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
