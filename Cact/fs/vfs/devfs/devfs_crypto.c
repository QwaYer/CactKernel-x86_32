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
extern int cact_crypt_random(uint8_t *buf, uint32_t len);
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
extern int cact_crypt_x25519_keygen(uint8_t *pub_out, uint8_t *priv_out);
extern int cact_crypt_x25519_derive(const uint8_t *priv_in, const uint8_t *peer_pub,
                                    uint8_t *shared_out);
extern int cact_crypt_p256_keygen(uint8_t *pub_out, uint8_t *priv_out);
extern int cact_crypt_p256_derive(const uint8_t *priv_in, const uint8_t *peer_pub,
                                  uint8_t *shared_out);

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
        rc = cact_crypt_random(tmp, a.len);
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
        if (!arg || copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        if (a.alg == CACT_CRYPT_KX_X25519) {
            rc = cact_crypt_x25519_keygen(a.pub, a.priv);
        } else if (a.alg == CACT_CRYPT_KX_P256) {
            rc = cact_crypt_p256_keygen(a.pub, a.priv);
        } else {
            return -EINVAL;
        }
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

    default:
        return -EINVAL;
    }
}

static int _crypto_status(void *p, char *buf, uint32_t size) {
    (void)p;
    const char *s =
        "device: crypto\n"
        "type: kernel crypto service\n"
        "algorithms: sha256 sha384 hmac-sha256 hmac-sha384 hkdf-sha256 hkdf-sha384\n"
        "             aes-128-gcm aes-256-gcm x25519 p-256 rdrand\n";
    uint32_t n = 0;
    while (s[n] && n < size - 1) { buf[n] = s[n]; n++; }
    buf[n] = '\0';
    return (int)n;
}

devfs_driver_t drv_crypto = {
    .ioctl  = _crypto_ioctl,
    .status = _crypto_status,
};
