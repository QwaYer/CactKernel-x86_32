#ifndef MTRR_H
#define MTRR_H

#include <stdint.h>

/*
 * IA-32 MTRR (Memory Type Range Register) driver — variable ranges only.
 *
 * Primary use case: mark the linear framebuffer as Write-Combining so the
 * CPU coalesces sequential 32-bit pixel writes into burst transfers across
 * the PCI bus instead of issuing one UC store per pixel (the boot identity
 * map sets PCD|PWT on every page above PCI_HOLE_START, which by default
 * pins the effective memory type to UC).
 *
 * The full programming sequence (Intel SDM Vol 3A § 11.11.8) — disable
 * caches, WBINVD, flush TLB, clear MTRR master enable, write the variable
 * range pair, re-enable — must run with IRQs disabled and is therefore
 * intended to be called only from early-boot setup.
 */

#define MTRR_TYPE_UC  0x00  /* Uncacheable                              */
#define MTRR_TYPE_WC  0x01  /* Write-Combining (requires CPUID support) */
#define MTRR_TYPE_WT  0x04  /* Write-Through                            */
#define MTRR_TYPE_WP  0x05  /* Write-Protected                          */
#define MTRR_TYPE_WB  0x06  /* Write-Back                               */

/*
 * Detect MTRR + WC capabilities. Logs the discovered geometry (variable
 * range count, MAXPHYSADDR, current default type, master enable) so the
 * boot transcript shows whether WC is actually viable on this CPU.
 * Safe to call before any framebuffer activity; only reads MSRs/CPUID.
 */
void mtrr_init(void);

/* 1 iff the CPU advertises MTRR support AND IA32_MTRRCAP.WC. */
int  mtrr_wc_available(void);

/*
 * Mark a physical range [base, base + size) as Write-Combining via one
 * or more variable MTRR pairs. `base` must be 4 KB aligned; `size` is
 * rounded up to a 4 KB multiple. Sizes/alignments that are not naturally
 * a single power of two are split greedily into the minimum number of
 * (pow2-aligned, pow2-sized) chunks; each chunk consumes one variable
 * MTRR slot.
 *
 * Returns the number of variable MTRR slots consumed, or:
 *   -1  MTRR or WC not supported by this CPU
 *   -2  invalid argument (size == 0 or base misaligned)
 *   -3  not enough free variable MTRR slots
 */
int  mtrr_add_wc(uint32_t base, uint32_t size);

/*
 * Convenience entry point for the framebuffer. Picks a single naturally
 * aligned power-of-two block that covers [fb_phys, fb_phys + pitch*height)
 * if `fb_phys` is aligned that way (typical for a PCI BAR); otherwise
 * falls back to the greedy decomposition. After the MTRR pair is live,
 * walks the kernel page tables and drops PCD|PWT from every PTE in the
 * range (and INVLPGs them) so the WC type can actually take effect.
 *
 * Returns 0 on success, negative on error (same codes as mtrr_add_wc).
 */
int  mtrr_enable_wc_for_framebuffer(uint32_t fb_phys,
                                    uint32_t fb_pitch,
                                    uint32_t fb_height);

#endif
