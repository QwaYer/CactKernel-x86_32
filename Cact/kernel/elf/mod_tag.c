#include "mod_tag.h"
#include "ksym.h"
#include "klib.h"
#include "kernel.h"
#include "cctkfs.h"
#include "mod_pubkey.h"

/* ECDSA P-256 / SHA-256 verify-only — implemented in cact_crypto (Rust, no_std). */
extern int cact_sig_verify_p256_raw(const uint8_t *pubkey, uint32_t pubkey_len,
                                    const uint8_t *msg, uint32_t msg_len,
                                    const uint8_t *sig, uint32_t sig_len);

CACT_STATIC_ASSERT(CACT_MOD_TAG_SIG_SIZE == 64);
CACT_STATIC_ASSERT(CACT_MODULE_PUBKEY_LEN == 65);

static uint32_t mod_tag_read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int mod_tag_verify(uint8_t *elf_data, uint32_t *file_size) {
    if (*file_size <= CACT_MOD_TAG_SIZE) {
        pr_err("[MODTAG] unsigned module (no signature) — rejected\n");
        return CACT_MOD_TAG_E_UNSIGNED;
    }

    uint32_t data_len = *file_size - CACT_MOD_TAG_SIZE;   /* ELF length */
    uint8_t *magic    = elf_data + data_len;
    uint8_t *vmagic   = magic + CACT_MOD_TAG_MAGIC_SIZE;
    uint8_t *sig      = vmagic + CACT_MOD_TAG_VERMAGIC_SIZE;

    if (magic[0] != CACT_MOD_TAG_MAGIC0 || magic[1] != CACT_MOD_TAG_MAGIC1 ||
        magic[2] != CACT_MOD_TAG_MAGIC2 || magic[3] != CACT_MOD_TAG_MAGIC3) {
        pr_err("[MODTAG] no module trailer (stale or unsigned) — rejected\n");
        return CACT_MOD_TAG_E_UNSIGNED;
    }

    /* The signature covers ELF || magic || vermagic. */
    if (cact_sig_verify_p256_raw(cact_module_pubkey, CACT_MODULE_PUBKEY_LEN,
                                 elf_data, data_len + CACT_MOD_TAG_MAGIC_SIZE
                                             + CACT_MOD_TAG_VERMAGIC_SIZE,
                                 sig, CACT_MOD_TAG_SIG_SIZE) != 0) {
        pr_err("[MODTAG] signature mismatch — rejected\n");
        return CACT_MOD_TAG_E_SIG;
    }

    uint32_t want = ksym_vermagic();
    uint32_t got  = mod_tag_read_le32(vmagic);
    if (got != want) {
        pr_err("[MODTAG] ABI mismatch: module vermagic 0x%x, kernel 0x%x — "
               "rebuild the module against this kernel\n",
               (unsigned)got, (unsigned)want);
        return CACT_MOD_TAG_E_VERMAGIC;
    }

    *file_size = data_len;
    for (uint32_t i = 0; i < CACT_MOD_TAG_SIZE; i++)
        elf_data[data_len + i] = 0;
    return CACT_MOD_TAG_OK;
}

int mod_sign_selftest(void) {
    /* 1. Known-answer signature: proves the compiled-in public key belongs to
     *    the private key the build host signs with. */
    if (cact_sig_verify_p256_raw(cact_module_pubkey, CACT_MODULE_PUBKEY_LEN,
                                 cact_modsign_selftest_msg,
                                 CACT_MODSIGN_SELFTEST_MSG_LEN,
                                 cact_modsign_selftest_sig,
                                 CACT_MOD_TAG_SIG_SIZE) != 0)
        return -1;

    /* 2. Full trailer parse on an embedded signed blob: magic, signature and
     *    vermagic all have to line up, so a drift between this kernel's ABI
     *    and the build-time signer fails here rather than silently rejecting
     *    every module at load. */
    uint8_t  buf[CACT_MODSIGN_SELFTEST_MOD_TOTAL];
    uint32_t n = sizeof(buf);
    memcpy(buf, cact_modsign_selftest_mod, sizeof(buf));
    if (mod_tag_verify(buf, &n) != CACT_MOD_TAG_OK)
        return -2;
    if (n != CACT_MODSIGN_SELFTEST_MOD_LEN)
        return -3;
    return 0;
}
