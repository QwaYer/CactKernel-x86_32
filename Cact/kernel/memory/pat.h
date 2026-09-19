#ifndef PAT_H
#define PAT_H

#include <stdint.h>

/*
 * Page Attribute Table (PAT) driver — per-PTE memory type via PAT bit.
 *
 * PAT supersedes variable-range MTRRs for fine-grained cache control.
 * Instead of programming physical-address-range registers (which require
 * disabling caches, WBINVD, TLB flush), PAT simply uses bit 7 of each PTE
 * together with the existing PCD (bit 4) and PWT (bit 3) to select one of
 * eight memory types programmed in the IA32_PAT MSR (0x277).
 *
 * Entry 4 — the one the framebuffer PTEs select — is programmed to WC.
 * The architectural reset value of that entry is WB, firmware is not required
 * to have changed it, and an S3 resume resets the MSR, so pat_init() sets it
 * explicitly on every (re)initialisation:
 *
 *   Index  PAT:PCD:PWT   Type    Encoding
 *   ------ ------------  ------- ---------
 *   0      0:0:0         WB      0x06    — Normal RAM (write-back)
 *   1      0:0:1         WT      0x04
 *   2      0:1:0         UC-     0x07
 *   3      0:1:1         UC      0x00    — MMIO (unchanged from legacy)
 *   4      1:0:0         WC      0x01    — Framebuffer write-combining
 *   5      1:0:1         WP      0x05
 *   6      1:1:0         UC-     0x07
 *   7      1:1:1         UC      0x00
 *
 * A variable-range MTRR still overrides PAT for the uncacheable case, so the
 * MTRR block firmware programmed is snapshotted and restored across S3 as
 * well (see mtrr.h): otherwise a device range the reset left uncacheable
 * could not be made write-combining by any PTE bit.
 *
 * Usage:
 *   1. Call pat_init() early during boot, and again on resume, to program
 *      the MSR.
 *   2. Call pat_enable_wc_for_framebuffer() to set PAT|~PCD|~PWT on FB PTEs.
 */

/* CPUID leaf 1 EDX bit 16 = PAT support. */
#define PAT_CPUID_EDX_BIT  16

/* PAT bit in PTE/PDE (bit 7). */
#define PAGE_PAT           0x80

/*
 * Detect PAT support via CPUID, program PAT entry 4 to Write-Combining, and
 * log the capability once.  Safe to call before any framebuffer activity, and
 * again after a resume (which silently restores the MSR).
 */
void pat_init(void);

/* 1 iff the CPU supports PAT (CPUID.01H:EDX.PAT=1). */
int  pat_available(void);

/*
 * Mark the framebuffer range as Write-Combining by setting the PAT bit
 * and clearing PCD|PWT on every PTE in [fb_phys, fb_phys + pitch*height).
 * That selects PAT entry 4, which pat_init() programs to WC.
 *
 * Also used after a resume to re-apply the bits and flush the stale
 * translations, so the framebuffer is not left write-back.
 *
 * Returns 0 on success, negative on error:
 *   -1  PAT not supported
 *   -2  invalid arguments
 */
int  pat_enable_wc_for_framebuffer(uint32_t fb_phys,
                                    uint32_t fb_pitch,
                                    uint32_t fb_height);

#endif
