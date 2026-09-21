//! Signature verification for the schemes TLS certificates use: ECDSA
//! P-256/P-384 and RSA PKCS#1 v1.5 / PSS.
//!
//! The public key is passed exactly as a certificate carries it — the contents
//! of the SubjectPublicKeyInfo BIT STRING, i.e. a SEC1 point for ECDSA and a
//! DER `RSAPublicKey` for RSA.  That is precisely what rustls-webpki hands to a
//! `SignatureVerificationAlgorithm`, so this one implementation serves both the
//! kernel's own chain validation (via the provider's algorithms) and the
//! `/dev/crypto` service that userspace asks.
//!
//! Verify-only on purpose: the kernel never signs, so it has no private keys to
//! protect and no signing side channels to worry about.

use const_oid::AssociatedOid;
use rsa::pkcs1::DecodeRsaPublicKey;
use sha2::digest::FixedOutputReset;
use sha2::{Digest, Sha256, Sha384, Sha512};
use signature::hazmat::PrehashVerifier;

/// Scheme selectors — mirrored in `ioctl_abi.h` as `CACT_SIG_*`.
pub const SIG_ECDSA_P256_SHA256: u32 = 0;
pub const SIG_ECDSA_P384_SHA384: u32 = 1;
pub const SIG_RSA_PKCS1_SHA256: u32 = 2;
pub const SIG_RSA_PKCS1_SHA384: u32 = 3;
pub const SIG_RSA_PKCS1_SHA512: u32 = 4;
pub const SIG_RSA_PSS_SHA256: u32 = 5;
pub const SIG_RSA_PSS_SHA384: u32 = 6;
pub const SIG_RSA_PSS_SHA512: u32 = 7;

/// Verify that `sig` is a valid signature over `msg` under `pubkey` for
/// `scheme`.  Any malformed input (bad point, bad DER signature, wrong scheme)
/// is simply "not valid" — callers get no oracle for *why* it failed.
pub fn verify(scheme: u32, pubkey: &[u8], msg: &[u8], sig: &[u8]) -> bool {
    match scheme {
        SIG_ECDSA_P256_SHA256 => ecdsa_p256(pubkey, &Sha256::digest(msg), sig),
        SIG_ECDSA_P384_SHA384 => ecdsa_p384(pubkey, &Sha384::digest(msg), sig),
        SIG_RSA_PKCS1_SHA256 => rsa_pkcs1::<Sha256>(pubkey, &Sha256::digest(msg), sig),
        SIG_RSA_PKCS1_SHA384 => rsa_pkcs1::<Sha384>(pubkey, &Sha384::digest(msg), sig),
        SIG_RSA_PKCS1_SHA512 => rsa_pkcs1::<Sha512>(pubkey, &Sha512::digest(msg), sig),
        SIG_RSA_PSS_SHA256 => rsa_pss::<Sha256>(pubkey, &Sha256::digest(msg), sig),
        SIG_RSA_PSS_SHA384 => rsa_pss::<Sha384>(pubkey, &Sha384::digest(msg), sig),
        SIG_RSA_PSS_SHA512 => rsa_pss::<Sha512>(pubkey, &Sha512::digest(msg), sig),
        _ => false,
    }
}

/// True when `scheme` names an algorithm this build can verify.
pub fn scheme_supported(scheme: u32) -> bool {
    scheme <= SIG_RSA_PSS_SHA512
}

fn ecdsa_p256(pubkey: &[u8], prehash: &[u8], sig: &[u8]) -> bool {
    let Ok(vk) = p256::ecdsa::VerifyingKey::from_sec1_bytes(pubkey) else {
        return false;
    };
    let Ok(sig) = p256::ecdsa::Signature::from_der(sig) else {
        return false;
    };
    vk.verify_prehash(prehash, &sig).is_ok()
}

fn ecdsa_p384(pubkey: &[u8], prehash: &[u8], sig: &[u8]) -> bool {
    let Ok(vk) = p384::ecdsa::VerifyingKey::from_sec1_bytes(pubkey) else {
        return false;
    };
    let Ok(sig) = p384::ecdsa::Signature::from_der(sig) else {
        return false;
    };
    vk.verify_prehash(prehash, &sig).is_ok()
}

fn rsa_pkcs1<D>(pubkey: &[u8], prehash: &[u8], sig: &[u8]) -> bool
where
    D: Digest + AssociatedOid,
{
    let Ok(key) = rsa::RsaPublicKey::from_pkcs1_der(pubkey) else {
        return false;
    };
    let Ok(sig) = rsa::pkcs1v15::Signature::try_from(sig) else {
        return false;
    };
    rsa::pkcs1v15::VerifyingKey::<D>::new(key)
        .verify_prehash(prehash, &sig)
        .is_ok()
}

fn rsa_pss<D>(pubkey: &[u8], prehash: &[u8], sig: &[u8]) -> bool
where
    D: Digest + FixedOutputReset,
{
    let Ok(key) = rsa::RsaPublicKey::from_pkcs1_der(pubkey) else {
        return false;
    };
    let Ok(sig) = rsa::pss::Signature::try_from(sig) else {
        return false;
    };
    // VerifyingKey::new uses the digest length as the salt length, which is
    // what TLS 1.3 RSA-PSS stipulates.
    rsa::pss::VerifyingKey::<D>::new(key)
        .verify_prehash(prehash, &sig)
        .is_ok()
}

// ──────────────────────────────── C ABI ────────────────────────────────────

unsafe fn cslice<'a>(p: *const u8, len: u32) -> Option<&'a [u8]> {
    if p.is_null() {
        if len == 0 {
            return Some(&[]);
        }
        return None;
    }
    Some(core::slice::from_raw_parts(p, len as usize))
}

/// C ABI for `/dev/crypto`.  Returns 0 when the signature is valid, -1 when it
/// is not, and -22 when the arguments are malformed or the scheme is unknown.
#[no_mangle]
pub extern "C" fn cact_sig_verify(
    scheme: u32,
    pubkey: *const u8,
    pubkey_len: u32,
    msg: *const u8,
    msg_len: u32,
    sig: *const u8,
    sig_len: u32,
) -> i32 {
    if !scheme_supported(scheme) {
        return -22;
    }
    // SAFETY: every pointer is validated by cslice before use.
    let parts = unsafe {
        (
            cslice(pubkey, pubkey_len),
            cslice(msg, msg_len),
            cslice(sig, sig_len),
        )
    };
    let (Some(pk), Some(m), Some(s)) = parts else {
        return -22;
    };
    if pk.is_empty() || s.is_empty() {
        return -22;
    }
    if verify(scheme, pk, m, s) {
        0
    } else {
        -1
    }
}
