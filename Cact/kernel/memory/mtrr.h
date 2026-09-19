#ifndef MTRR_H
#define MTRR_H

/*
 * MTRR (memory type range register) state, kept across an S3 suspend.
 *
 * The MTRRs are a per-CPU MSR block that firmware programs once at boot and
 * that overrides the PAT for the "uncacheable" case: an address outside every
 * variable range takes the default type, and a default of UC cannot be made
 * cacheable again by any PTE/PAT bit.  A platform reset resets the block to
 * its architectural default and does not re-run the firmware that set it up,
 * so the OS has to restore it on resume — the same way it restores PCI
 * configuration space.
 */
void mtrr_save(void);        /* snapshot, once, after firmware set the ranges */
void mtrr_restore(void);     /* re-program the snapshot after a resume         */

#endif
