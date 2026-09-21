#include "devfs.h"
#include "devfs_internal.h"
#include "vfs.h"
#include "memory.h"
#include "klib.h"
#include "kernel.h"
#include "validate.h"
#include "ioctl_abi.h"

// devfs_crypto.c — /dev/crypto, a kernel-service device exposing the
// in-kernel crypto primitives (the same algorithms the rustls cact_crypto
// provider ships: SHA-256/384, HMAC, HKDF, AES-GCM, X25519, P-256, RDRAND)
// to userspace.  All algorithms run in the Rust cact_hmac_ffi crate; this
// file only copies ioctl buffers across the user/kernel boundary.
//
//   /dev/crypto  ioctl  -> random/hash/hmac/hkdf/aead/key-exchange

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EFAULT
#define EFAULT 14
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif

// cact_crypto userspace FFI (Rust, cact_hmac_ffi).  Return 0 on success,
// negative otherwise.
extern int cact_crypt_hash(uint32_t alg, const uint8_t *data, uint32_t data_len,
                           uint8_t *digest);
extern int cact_crypt_hmac_sign(uint32_t alg, const uint8_t *key, uint32_t key_len,
                                const uint8_t *data, uint32_t data_len, uint8_t *tag);
extern int cact_crypt_hmac_verify(uint32_t alg, const uint8_t *key, uint32_t key_len,
                                  const uint8_t *data, uint32_t data_len,
                                  const uint8_t *tag);
extern int cact_crypt_hkdf(uint32_t alg, const uint8_t *salt, uint32_t salt_len,
                           const uint8_t *ikm, uint32_t ikm_len,
                           const uint8_t *info, uint32_t info_len,
                           uint8_t *out, uint32_t out_len);
extern int cact_crypt_aead(uint32_t alg, uint32_t op,
                           const uint8_t *key, uint32_t key_len,
                           const uint8_t *nonce, const uint8_t *aad, uint32_t aad_len,
                           uint32_t in_len, uint8_t *buf, uint32_t cap,
                           uint32_t *out_len);
// Key material comes from the kernel CSPRNG (cact_crypto), never from a bare
// RDRAND instruction.
extern int cact_csprng_fill(uint8_t *buf, uint32_t len);
extern int cact_crypt_x25519_keygen(const uint8_t *seed, uint32_t seed_len,
                                    uint8_t *pub_out, uint8_t *priv_out);
extern int cact_crypt_x25519_derive(const uint8_t *priv_in, const uint8_t *peer_pub,
                                    uint8_t *shared_out);
extern int cact_crypt_p256_keygen(const uint8_t *seed, uint32_t seed_len,
                                  uint8_t *pub_out, uint8_t *priv_out);
extern int cact_crypt_p256_derive(const uint8_t *priv_in, const uint8_t *peer_pub,
                                  uint8_t *shared_out);
// Signature verification (cact_crypto/src/sig.rs).  Takes kernel pointers; the
// ioctl handler copies the user buffers in first.
extern int cact_sig_verify(uint32_t scheme, const uint8_t *pubkey, uint32_t pubkey_len,
                           const uint8_t *msg, uint32_t msg_len,
                           const uint8_t *sig, uint32_t sig_len);
// Certificate chain verification (cact_crypto/src/x509.rs); kernel pointers, as
// above.
extern int cact_x509_verify(const uint8_t *chain, uint32_t chain_len,
                            const uint8_t *roots, uint32_t roots_len,
                            const char *hostname, uint64_t unix_time,
                            uint32_t tls_scheme,
                            const uint8_t *hs_msg, uint32_t hs_msg_len,
                            const uint8_t *hs_sig, uint32_t hs_sig_len);
// Per-operation cap on dynamic data buffers (avoid unbounded kernel copies).
#define CRYPT_MAX_DATA 0x100000u

// copy a user buffer into a fresh kernel buffer; NULL/zero-length-safe.
static uint8_t *_copy_in(const void *user, uint32_t len) {
    if (len > CRYPT_MAX_DATA) return 0;
    if (len && (!user || !validate_user_ptr(user, len))) return 0;
    uint8_t *kbuf = (uint8_t *)kmalloc(len ? len : 1);
    if (!kbuf) return 0;
    if (len) memcpy(kbuf, user, len);
    return kbuf;
}

// copy a kernel buffer back to a validated user buffer.
static int _copy_out(void *user, const void *src, uint32_t len) {
    if (len && !validate_user_ptr(user, len)) return -1;
    if (len) memcpy(user, src, len);
    return 0;
}

static int _crypto_ioctl(void *p, uint32_t cmd, void *arg) {
    (void)p;
    int rc;

    switch (cmd) {

    case CACT_CRYPTCTL_RANDOM: {
        cact_crypt_random_arg_t a;
        if (!arg || copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        if (a.len > CRYPT_MAX_DATA) return -EINVAL;
        if (a.len && !validate_user_ptr(a.buf, a.len)) return -EFAULT;
        uint8_t *tmp = (uint8_t *)kmalloc(a.len ? a.len : 1);
        if (!tmp) return -ENOMEM;
        rc = cact_csprng_fill(tmp, a.len);
        if (rc == 0) rc = _copy_out(a.buf, tmp, a.len);
        kfree(tmp);
        return rc;
    }

    case CACT_CRYPTCTL_HASH: {
        cact_crypt_hash_arg_t a;
        if (!arg || copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        uint8_t *data = _copy_in(a.data, a.data_len);
        if (a.data_len && !data) return -EFAULT;
        rc = cact_crypt_hash(a.alg, data, a.data_len, a.digest);
        if (data) kfree(data);
        if (rc != 0) return rc;
        return copy_to_user(arg, &a, sizeof(a));
    }

    case CACT_CRYPTCTL_HMAC: {
        cact_crypt_hmac_arg_t a;
        if (!arg || copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        uint8_t *key = _copy_in(a.key, a.key_len);
        if (a.key_len && !key) return -EFAULT;
        uint8_t *data = _copy_in(a.data, a.data_len);
        if (a.data_len && !data) { kfree(key); return -EFAULT; }
        rc = cact_crypt_hmac_sign(a.alg, key, a.key_len, data, a.data_len, a.tag);
        kfree(data);
        kfree(key);
        if (rc != 0) return rc;
        return copy_to_user(arg, &a, sizeof(a));
    }

    case CACT_CRYPTCTL_HMAC_VERIFY: {
        cact_crypt_hmac_arg_t a;
        if (!arg || copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        uint8_t *key = _copy_in(a.key, a.key_len);
        if (a.key_len && !key) return -EFAULT;
        uint8_t *data = _copy_in(a.data, a.data_len);
        if (a.data_len && !data) { kfree(key); return -EFAULT; }
        rc = cact_crypt_hmac_verify(a.alg, key, a.key_len, data, a.data_len, a.tag);
        kfree(data);
        kfree(key);
        return rc;   // 0 = tag matches
    }

    case CACT_CRYPTCTL_HKDF: {
        cact_crypt_hkdf_arg_t a;
        if (!arg || copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        if (a.out_len > CRYPT_MAX_DATA) return -EINVAL;
        if (a.out_len && !validate_user_ptr(a.out, a.out_len)) return -EFAULT;
        uint8_t *salt = _copy_in(a.salt, a.salt_len);
        if (a.salt_len && !salt) return -EFAULT;
        uint8_t *ikm = _copy_in(a.ikm, a.ikm_len);
        if (a.ikm_len && !ikm) { kfree(salt); return -EFAULT; }
        uint8_t *info = _copy_in(a.info, a.info_len);
        if (a.info_len && !info) { kfree(ikm); kfree(salt); return -EFAULT; }
        uint8_t *out = (uint8_t *)kmalloc(a.out_len ? a.out_len : 1);
        if (!out) { kfree(info); kfree(ikm); kfree(salt); return -ENOMEM; }
        rc = cact_crypt_hkdf(a.alg, salt, a.salt_len, ikm, a.ikm_len,
                             info, a.info_len, out, a.out_len);
        kfree(info);
        kfree(ikm);
        kfree(salt);
        if (rc == 0) rc = _copy_out(a.out, out, a.out_len);
        kfree(out);
        return rc;
    }

    case CACT_CRYPTCTL_AEAD: {
        cact_crypt_aead_arg_t a;
        if (!arg || copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        if (a.in_len > CRYPT_MAX_DATA) return -EINVAL;
        if (a.op == CACT_CRYPT_OP_SEAL) {
            if (a.out_cap < a.in_len + CACT_CRYPT_GCM_TAG_LEN) return -EINVAL;
        } else {
            if (a.op != CACT_CRYPT_OP_OPEN) return -EINVAL;
            if (a.in_len < CACT_CRYPT_GCM_TAG_LEN) return -EINVAL;
            if (a.out_cap < a.in_len - CACT_CRYPT_GCM_TAG_LEN) return -EINVAL;
        }
        uint32_t cap = (a.op == CACT_CRYPT_OP_SEAL)
                       ? a.in_len + CACT_CRYPT_GCM_TAG_LEN
                       : a.in_len;
        uint8_t *key = _copy_in(a.key, a.key_len);
        if (a.key_len && !key) return -EFAULT;
        uint8_t *aad = _copy_in(a.aad, a.aad_len);
        if (a.aad_len && !aad) { kfree(key); return -EFAULT; }
        uint8_t *scratch = (uint8_t *)kmalloc(cap ? cap : 1);
        if (!scratch) { kfree(aad); kfree(key); return -ENOMEM; }
        if (a.in_len && (!a.in || copy_from_user(scratch, a.in, a.in_len) != 0)) {
            kfree(scratch); kfree(aad); kfree(key);
            return -EFAULT;
        }
        uint32_t out_len = 0;
        rc = cact_crypt_aead(a.alg, a.op, key, a.key_len, a.nonce,
                             aad, a.aad_len, a.in_len, scratch, cap, &out_len);
        kfree(aad);
        kfree(key);
        if (rc == 0) {
            if (out_len > a.out_cap) { kfree(scratch); return -EINVAL; }
            rc = _copy_out(a.out, scratch, out_len);
            if (rc == 0) {
                a.out_len = out_len;
                rc = copy_to_user(arg, &a, sizeof(a));
            }
        }
        kfree(scratch);
        return rc;
    }

    case CACT_CRYPTCTL_KX_KEYGEN: {
        cact_crypt_kx_keygen_arg_t a;
        uint8_t seed[64];
        if (!arg || copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        if (cact_csprng_fill(seed, sizeof(seed)) != 0) return -1;
        if (a.alg == CACT_CRYPT_KX_X25519) {
            rc = cact_crypt_x25519_keygen(seed, sizeof(seed), a.pub, a.priv);
        } else if (a.alg == CACT_CRYPT_KX_P256) {
            rc = cact_crypt_p256_keygen(seed, sizeof(seed), a.pub, a.priv);
        } else {
            memset(seed, 0, sizeof(seed));
            return -EINVAL;
        }
        memset(seed, 0, sizeof(seed));
        if (rc != 0) return rc;
        return copy_to_user(arg, &a, sizeof(a));
    }

    case CACT_CRYPTCTL_KX_DERIVE: {
        cact_crypt_kx_derive_arg_t a;
        if (!arg || copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        if (a.alg == CACT_CRYPT_KX_X25519) {
            rc = cact_crypt_x25519_derive(a.priv, a.peer_pub, a.shared);
        } else if (a.alg == CACT_CRYPT_KX_P256) {
            rc = cact_crypt_p256_derive(a.priv, a.peer_pub, a.shared);
        } else {
            return -EINVAL;
        }
        if (rc != 0) return rc;
        return copy_to_user(arg, &a, sizeof(a));
    }

    case CACT_CRYPTCTL_SIG_VERIFY: {
        cact_crypt_sig_verify_arg_t a;
        if (!arg || copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        // The Rust verifier works on kernel memory, so everything is copied in
        // (and validated) through _copy_in first.
        uint8_t *pubkey = _copy_in(a.pubkey, a.pubkey_len);
        if (!pubkey) return -EFAULT;
        uint8_t *msg = _copy_in(a.msg, a.msg_len);
        if (!msg) { kfree(pubkey); return -EFAULT; }
        uint8_t *sig = _copy_in(a.sig, a.sig_len);
        if (!sig) { kfree(msg); kfree(pubkey); return -EFAULT; }
        rc = cact_sig_verify(a.scheme, pubkey, a.pubkey_len,
                             msg, a.msg_len, sig, a.sig_len);
        kfree(sig);
        kfree(msg);
        kfree(pubkey);
        return rc;      // 0 = valid, -1 = invalid, -EINVAL = malformed
    }

    case CACT_CRYPTCTL_X509_VERIFY: {
        cact_crypt_x509_verify_arg_t a;
        uint8_t *chain, *roots, *msg, *sig;
        char *host;
        if (!arg || copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        if (!a.hostname || !validate_user_str(a.hostname)) return -EFAULT;
        host = copy_path_from_user(a.hostname);
        if (!host) return -EFAULT;
        chain = _copy_in(a.chain, a.chain_len);
        roots = _copy_in(a.roots, a.roots_len);
        msg   = _copy_in(a.hs_msg, a.hs_msg_len);
        sig   = _copy_in(a.hs_sig, a.hs_sig_len);
        if (!chain || !roots || !msg || !sig) {
            if (chain) kfree(chain);
            if (roots) kfree(roots);
            if (msg) kfree(msg);
            if (sig) kfree(sig);
            kfree(host);
            return -EFAULT;
        }
        rc = cact_x509_verify(chain, a.chain_len, roots, a.roots_len,
                              host, a.unix_time, a.tls_scheme,
                              msg, a.hs_msg_len, sig, a.hs_sig_len);
        kfree(chain);
        kfree(roots);
        kfree(msg);
        kfree(sig);
        kfree(host);
        return rc;
    }

    default:
        pr_err("  %-11s : unknown ioctl 0x%x\n", "crypto", cmd);
        return -EINVAL;
    }
}

devfs_driver_t drv_crypto = {
    .ioctl  = _crypto_ioctl,
};
