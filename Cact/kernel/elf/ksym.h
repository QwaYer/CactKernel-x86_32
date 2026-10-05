#ifndef KSYM_H
#define KSYM_H

#include <stdint.h>

uint32_t ksym_resolve(const char* name);

/* Module ABI fingerprint ("vermagic"): a 32-bit FNV-1a hash over the sorted
 * exported symbol names.  A .cctk carries this value in its signed tag and the
 * loader refuses a module whose value differs, i.e. one built against a kernel
 * with a different export set.  tools/mod_vermagic.py computes the identical
 * value from ksym.c; keep the two in lockstep. */
uint32_t ksym_vermagic(void);

#endif
