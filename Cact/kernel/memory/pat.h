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
 * Default IA32_PAT reset mapping (used as-is — no MSR reprogramming):
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
 * Usage:
 *   1. Call pat_init() early during boot to detect support.
 *   2. Call pat_enable_wc_for_framebuffer() to set PAT|~PCD|~PWT on FB PTEs.
 *
 * No MTRR ranges are programmed; no global cache flush is needed.
 */

/* CPUID leaf 1 EDX bit 16 = PAT support. */
#define PAT_CPUID_EDX_BIT  16

/* PAT bit in PTE/PDE (bit 7). */
#define PAGE_PAT           0x80

/*
 * Detect PAT support via CPUID. Logs the result.
 * Safe to call before any framebuffer activity; only reads CPUID.
 */
void pat_init(void);

/* 1 iff the CPU supports PAT (CPUID.01H:EDX.PAT=1). */
int  pat_available(void);

/*
 * Mark the framebuffer range as Write-Combining by setting the PAT bit
 * and clearing PCD|PWT on every PTE in [fb_phys, fb_phys + pitch*height).
 *
 * With the default PAT MSR mapping, (PAT=1, PCD=0, PWT=0) selects
 * PAT entry 4 = WC (Write-Combining).
 *
 * Returns 0 on success, negative on error:
 *   -1  PAT not supported
 *   -2  invalid arguments
 */
int  pat_enable_wc_for_framebuffer(uint32_t fb_phys,
                                    uint32_t fb_pitch,
                                    uint32_t fb_height);

#endif
