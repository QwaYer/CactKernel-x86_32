#ifndef CACT_MOD_TAG_H
#define CACT_MOD_TAG_H

#include <stdint.h>

/*
 * Signed-module trailer (see tools/modsign.py):
 *
 *     [ ELF image ][ magic:4 ][ vermagic:4 LE ][ ECDSA-P256 signature:64 ]
 *
 * The signature is ECDSA P-256 over SHA-256(ELF || magic || vermagic), in
 * fixed r||s form.  Only the public key is embedded in the kernel, so a leaked
 * kernel image cannot be used to forge a module; the 4-byte vermagic binds the
 * module to the kernel's exported-symbol ABI (see ksym_vermagic()).
 */
#define CACT_MOD_TAG_MAGIC_SIZE     4
#define CACT_MOD_TAG_VERMAGIC_SIZE  4
#define CACT_MOD_TAG_SIG_SIZE      64
#define CACT_MOD_TAG_SIZE \
    (CACT_MOD_TAG_MAGIC_SIZE + CACT_MOD_TAG_VERMAGIC_SIZE + CACT_MOD_TAG_SIG_SIZE)

/* "CMOD" — little-endian bytes 'C','M','O','D'. */
#define CACT_MOD_TAG_MAGIC0 'C'
#define CACT_MOD_TAG_MAGIC1 'M'
#define CACT_MOD_TAG_MAGIC2 'O'
#define CACT_MOD_TAG_MAGIC3 'D'

#define CACT_MOD_TAG_OK           0
#define CACT_MOD_TAG_E_UNSIGNED  -1   /* no usable trailer                 */
#define CACT_MOD_TAG_E_SIG       -2   /* signature does not verify         */
#define CACT_MOD_TAG_E_VERMAGIC  -3   /* built against a different ABI     */

/*
 * Verify a signed module in place.  On success *file_size is reduced to the
 * ELF-image length (the trailer bytes are zeroed, so section-header reads past
 * the end see zeros).  Returns CACT_MOD_TAG_OK or a negative CACT_MOD_TAG_E_*;
 * a failure is logged with the reason.
 */
int mod_tag_verify(uint8_t *elf_data, uint32_t *file_size);

/*
 * Verify the embedded known-answer signature at boot: proves the public key in
 * the kernel image and the signing key actually match.  Returns 0 when the
 * signature verifies, non-zero otherwise.
 */
int mod_sign_selftest(void);

#endif /* CACT_MOD_TAG_H */
