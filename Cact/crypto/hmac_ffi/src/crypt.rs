// crypt.rs — /dev/crypto userspace FFI (linked into the kernel via the
// cact_hmac_ffi staticlib).
//
// One-shot, allocation-free primitives mirroring the algorithms the rustls
// cact_crypto provider ships (SHA-256/384, HMAC, HKDF, AES-GCM, X25519,
// P-256, RDRAND).  They operate on kernel-owned buffers that the devfs
// driver has already copied from / to user space.
//
// Return values: 0 = success, negative = -errno style error (-1 generic
// failure / tag mismatch, -22 invalid argument).

use sha2::{Digest, Sha256, Sha384};
use hkdf::Hkdf;
use aes_gcm::aead::{AeadInPlace, KeyInit};
use aes_gcm::{Aes128Gcm, Aes256Gcm};
use x25519_dalek::{PublicKey as XPublicKey, StaticSecret};
use p256::elliptic_curve::sec1::ToEncodedPoint;

const CRYPT_EINVAL: i32 = -22;
const CRYPT_FAIL: i32 = -1;

fn crypt_rdrand32(out: &mut [u8]) -> bool {
    let mut off = 0usize;
    while off < out.len() {
        #[cfg(target_arch = "x86")]
        {
            let mut r: u32 = 0;
            let ret = unsafe { core::arch::x86::_rdrand32_step(&mut r) };
            if ret != 1 {
                return false;
            }
            let n = if out.len() - off < 4 { out.len() - off } else { 4 };
            out[off..off + n].copy_from_slice(&r.to_le_bytes()[..n]);
            off += n;
        }
        #[cfg(not(target_arch = "x86"))]
        {
            return false;
        }
    }
    true
}

unsafe fn crypt_slice<'a>(p: *const u8, len: u32) -> Option<&'a [u8]> {
    if p.is_null() {
        if len == 0 {
            return Some(&[]);
        }
        return None;
    }
    Some(core::slice::from_raw_parts(p, len as usize))
}

/// Fill a buffer with hardware random bytes (RDRAND).
#[no_mangle]
pub extern "C" fn cact_crypt_random(buf: *mut u8, len: u32) -> i32 {
    if buf.is_null() {
        return CRYPT_EINVAL;
    }
    let out = unsafe { core::slice::from_raw_parts_mut(buf, len as usize) };
    if crypt_rdrand32(out) {
        0
    } else {
        CRYPT_FAIL
    }
}

/// One-shot SHA-256 (alg=0, digest 32) / SHA-384 (alg=1, digest 48).
#[no_mangle]
pub extern "C" fn cact_crypt_hash(
    alg: u32,
    data: *const u8,
    data_len: u32,
    digest: *mut u8,
) -> i32 {
    if digest.is_null() {
        return CRYPT_EINVAL;
    }
    let data_slice = match unsafe { crypt_slice(data, data_len) } {
        Some(s) => s,
        None => return CRYPT_EINVAL,
    };
    match alg {
        0 => {
            let mut h = Sha256::new();
            h.update(data_slice);
            let out = h.finalize();
            unsafe { core::ptr::copy_nonoverlapping(out.as_ptr(), digest, 32) };
            0
        }
        1 => {
            let mut h = Sha384::new();
            h.update(data_slice);
            let out = h.finalize();
            unsafe { core::ptr::copy_nonoverlapping(out.as_ptr(), digest, 48) };
            0
        }
        _ => CRYPT_EINVAL,
    }
}

type HmacSha256 = hmac::Hmac<Sha256>;
type HmacSha384 = hmac::Hmac<Sha384>;

/// Compute HMAC-SHA256 (alg=0, tag 32) / HMAC-SHA384 (alg=1, tag 48).
#[no_mangle]
pub extern "C" fn cact_crypt_hmac_sign(
    alg: u32,
    key: *const u8,
    key_len: u32,
    data: *const u8,
    data_len: u32,
    tag: *mut u8,
) -> i32 {
    if tag.is_null() {
        return CRYPT_EINVAL;
    }
    let key_slice = match unsafe { crypt_slice(key, key_len) } {
        Some(s) => s,
        None => return CRYPT_EINVAL,
    };
    let data_slice = match unsafe { crypt_slice(data, data_len) } {
        Some(s) => s,
        None => return CRYPT_EINVAL,
    };
    match alg {
        0 => {
            let mut mac = match <HmacSha256 as hmac::Mac>::new_from_slice(key_slice) {
                Ok(m) => m,
                Err(_) => return CRYPT_EINVAL,
            };
            <HmacSha256 as hmac::Mac>::update(&mut mac, data_slice);
            let out = <HmacSha256 as hmac::Mac>::finalize(mac).into_bytes();
            unsafe { core::ptr::copy_nonoverlapping(out.as_ptr(), tag, 32) };
            0
        }
        1 => {
            let mut mac = match <HmacSha384 as hmac::Mac>::new_from_slice(key_slice) {
                Ok(m) => m,
                Err(_) => return CRYPT_EINVAL,
            };
            <HmacSha384 as hmac::Mac>::update(&mut mac, data_slice);
            let out = <HmacSha384 as hmac::Mac>::finalize(mac).into_bytes();
            unsafe { core::ptr::copy_nonoverlapping(out.as_ptr(), tag, 48) };
            0
        }
        _ => CRYPT_EINVAL,
    }
}

/// Constant-time verify of HMAC tag (32 bytes for alg=0, 48 for alg=1).
#[no_mangle]
pub extern "C" fn cact_crypt_hmac_verify(
    alg: u32,
    key: *const u8,
    key_len: u32,
    data: *const u8,
    data_len: u32,
    tag: *const u8,
) -> i32 {
    let key_slice = match unsafe { crypt_slice(key, key_len) } {
        Some(s) => s,
        None => return CRYPT_EINVAL,
    };
    let data_slice = match unsafe { crypt_slice(data, data_len) } {
        Some(s) => s,
        None => return CRYPT_EINVAL,
    };
    let tag_len = match alg {
        0 => 32u32,
        1 => 48u32,
        _ => return CRYPT_EINVAL,
    };
    let tag_slice = match unsafe { crypt_slice(tag, tag_len) } {
        Some(s) => s,
        None => return CRYPT_EINVAL,
    };
    let ok = match alg {
        0 => {
            let mut mac = match <HmacSha256 as hmac::Mac>::new_from_slice(key_slice) {
                Ok(m) => m,
                Err(_) => return CRYPT_EINVAL,
            };
            <HmacSha256 as hmac::Mac>::update(&mut mac, data_slice);
            <HmacSha256 as hmac::Mac>::verify_slice(mac, tag_slice).is_ok()
        }
        1 => {
            let mut mac = match <HmacSha384 as hmac::Mac>::new_from_slice(key_slice) {
                Ok(m) => m,
                Err(_) => return CRYPT_EINVAL,
            };
            <HmacSha384 as hmac::Mac>::update(&mut mac, data_slice);
            <HmacSha384 as hmac::Mac>::verify_slice(mac, tag_slice).is_ok()
        }
        _ => return CRYPT_EINVAL,
    };
    if ok {
        0
    } else {
        CRYPT_FAIL
    }
}

/// HKDF extract+expand: alg=0 -> SHA-256 (out <= 8160), alg=1 -> SHA-384
/// (out <= 12240).  salt/info may be NULL with length 0.
#[no_mangle]
pub extern "C" fn cact_crypt_hkdf(
    alg: u32,
    salt: *const u8,
    salt_len: u32,
    ikm: *const u8,
    ikm_len: u32,
    info: *const u8,
    info_len: u32,
    out: *mut u8,
    out_len: u32,
) -> i32 {
    if out.is_null() {
        return CRYPT_EINVAL;
    }
    let salt_slice = match unsafe { crypt_slice(salt, salt_len) } {
        Some(s) => s,
        None => return CRYPT_EINVAL,
    };
    let ikm_slice = match unsafe { crypt_slice(ikm, ikm_len) } {
        Some(s) => s,
        None => return CRYPT_EINVAL,
    };
    let info_slice = match unsafe { crypt_slice(info, info_len) } {
        Some(s) => s,
        None => return CRYPT_EINVAL,
    };
    let out_slice = unsafe { core::slice::from_raw_parts_mut(out, out_len as usize) };
    let salt_opt = if salt_slice.is_empty() { None } else { Some(salt_slice) };
    match alg {
        0 => {
            if out_len as usize > 255 * 32 {
                return CRYPT_EINVAL;
            }
            let hk = Hkdf::<Sha256>::new(salt_opt, ikm_slice);
            if hk.expand(info_slice, out_slice).is_ok() {
                0
            } else {
                CRYPT_EINVAL
            }
        }
        1 => {
            if out_len as usize > 255 * 48 {
                return CRYPT_EINVAL;
            }
            let hk = Hkdf::<Sha384>::new(salt_opt, ikm_slice);
            if hk.expand(info_slice, out_slice).is_ok() {
                0
            } else {
                CRYPT_EINVAL
            }
        }
        _ => CRYPT_EINVAL,
    }
}

/// AES-GCM seal/open in place.
///
/// `buf` is a kernel scratch buffer that already holds the input at offset 0:
/// plaintext of length `in_len` for SEAL, ciphertext||tag of length `in_len`
/// for OPEN.  On success the output (ciphertext||tag or plaintext) replaces it
/// and its length is stored to `*out_len`.  `cap` is the scratch capacity and
/// must be >= in_len + 16 for SEAL.
fn crypt_gcm<C>(
    seal: bool,
    key: &[u8],
    nonce: &[u8],
    aad: &[u8],
    buf: &mut [u8],
    in_len: usize,
) -> Result<usize, ()>
where
    C: KeyInit + AeadInPlace,
{
    if nonce.len() != 12 {
        return Err(());
    }
    let cipher = <C as KeyInit>::new_from_slice(key).map_err(|_| ())?;
    let nonce = aes_gcm::aead::Nonce::<C>::from_slice(nonce);
    if seal {
        if buf.len() < in_len + 16 {
            return Err(());
        }
        let tag = cipher
            .encrypt_in_place_detached(nonce, aad, &mut buf[..in_len])
            .map_err(|_| ())?;
        buf[in_len..in_len + 16].copy_from_slice(tag.as_slice());
        Ok(in_len + 16)
    } else {
        if in_len < 16 {
            return Err(());
        }
        let ct_len = in_len - 16;
        let mut tag_arr = [0u8; 16];
        tag_arr.copy_from_slice(&buf[in_len - 16..]);
        let tag = aes_gcm::aead::Tag::<C>::from_slice(&tag_arr);
        cipher
            .decrypt_in_place_detached(nonce, aad, &mut buf[..ct_len], tag)
            .map_err(|_| ())?;
        Ok(ct_len)
    }
}

#[no_mangle]
pub extern "C" fn cact_crypt_aead(
    alg: u32,
    op: u32,
    key: *const u8,
    key_len: u32,
    nonce: *const u8,
    aad: *const u8,
    aad_len: u32,
    in_len: u32,
    buf: *mut u8,
    cap: u32,
    out_len: *mut u32,
) -> i32 {
    if out_len.is_null() || buf.is_null() || nonce.is_null() {
        return CRYPT_EINVAL;
    }
    if op != 0 && op != 1 {
        return CRYPT_EINVAL; // only CACT_CRYPT_OP_SEAL / CACT_CRYPT_OP_OPEN
    }
    let seal = op == 0;
    let key_slice = match unsafe { crypt_slice(key, key_len) } {
        Some(s) => s,
        None => return CRYPT_EINVAL,
    };
    let aad_slice = match unsafe { crypt_slice(aad, aad_len) } {
        Some(s) => s,
        None => return CRYPT_EINVAL,
    };
    let nonce_slice = match unsafe { crypt_slice(nonce, 12) } {
        Some(s) => s,
        None => return CRYPT_EINVAL,
    };
    let buf_slice = unsafe { core::slice::from_raw_parts_mut(buf, cap as usize) };
    let result = match alg {
        0 => crypt_gcm::<Aes128Gcm>(seal, key_slice, nonce_slice, aad_slice, buf_slice, in_len as usize),
        1 => crypt_gcm::<Aes256Gcm>(seal, key_slice, nonce_slice, aad_slice, buf_slice, in_len as usize),
        _ => return CRYPT_EINVAL,
    };
    match result {
        Ok(n) => {
            unsafe { *out_len = n as u32 };
            0
        }
        Err(()) => CRYPT_FAIL,
    }
}

/// Generate an X25519 key pair: priv is raw (clamped on use), pub 32 bytes.
#[no_mangle]
pub extern "C" fn cact_crypt_x25519_keygen(pub_out: *mut u8, priv_out: *mut u8) -> i32 {
    if pub_out.is_null() || priv_out.is_null() {
        return CRYPT_EINVAL;
    }
    let mut priv_bytes = [0u8; 32];
    if !crypt_rdrand32(&mut priv_bytes) {
        return CRYPT_FAIL;
    }
    let sk = StaticSecret::from(priv_bytes);
    let pk = XPublicKey::from(&sk);
    unsafe {
        core::ptr::copy_nonoverlapping(pk.as_bytes().as_ptr(), pub_out, 32);
        core::ptr::copy_nonoverlapping(priv_bytes.as_ptr(), priv_out, 32);
    }
    0
}

/// X25519 shared secret from local priv (32) and peer pub (32).
#[no_mangle]
pub extern "C" fn cact_crypt_x25519_derive(
    priv_in: *const u8,
    peer_pub: *const u8,
    shared_out: *mut u8,
) -> i32 {
    if priv_in.is_null() || peer_pub.is_null() || shared_out.is_null() {
        return CRYPT_EINVAL;
    }
    let mut priv_bytes = [0u8; 32];
    let mut peer_bytes = [0u8; 32];
    unsafe {
        core::ptr::copy_nonoverlapping(priv_in, priv_bytes.as_mut_ptr(), 32);
        core::ptr::copy_nonoverlapping(peer_pub, peer_bytes.as_mut_ptr(), 32);
    }
    let sk = StaticSecret::from(priv_bytes);
    let pk = XPublicKey::from(peer_bytes);
    let shared = sk.diffie_hellman(&pk);
    unsafe {
        core::ptr::copy_nonoverlapping(shared.as_bytes().as_ptr(), shared_out, 32);
    }
    0
}

/// Generate a P-256 (secp256r1) key pair: pub is 65-byte uncompressed SEC1.
#[no_mangle]
pub extern "C" fn cact_crypt_p256_keygen(pub_out: *mut u8, priv_out: *mut u8) -> i32 {
    if pub_out.is_null() || priv_out.is_null() {
        return CRYPT_EINVAL;
    }
    let mut attempts = 0u32;
    let (priv_bytes, sk) = loop {
        let mut priv_bytes = [0u8; 32];
        if !crypt_rdrand32(&mut priv_bytes) {
            return CRYPT_FAIL;
        }
        let mut fb = p256::FieldBytes::default();
        fb.copy_from_slice(&priv_bytes);
        if let Ok(sk) = p256::SecretKey::from_bytes(&fb) {
            break (priv_bytes, sk);
        }
        attempts += 1;
        if attempts > 16 {
            return CRYPT_FAIL;
        }
    };
    let pk = sk.public_key();
    let ep = pk.to_encoded_point(false);
    if ep.as_bytes().len() != 65 {
        return CRYPT_FAIL;
    }
    unsafe {
        core::ptr::copy_nonoverlapping(ep.as_bytes().as_ptr(), pub_out, 65);
        core::ptr::copy_nonoverlapping(priv_bytes.as_ptr(), priv_out, 32);
    }
    0
}

/// P-256 shared secret from local priv (32) and peer pub (65 uncompressed).
#[no_mangle]
pub extern "C" fn cact_crypt_p256_derive(
    priv_in: *const u8,
    peer_pub: *const u8,
    shared_out: *mut u8,
) -> i32 {
    if priv_in.is_null() || peer_pub.is_null() || shared_out.is_null() {
        return CRYPT_EINVAL;
    }
    let mut priv_bytes = [0u8; 32];
    let mut peer_bytes = [0u8; 65];
    unsafe {
        core::ptr::copy_nonoverlapping(priv_in, priv_bytes.as_mut_ptr(), 32);
        core::ptr::copy_nonoverlapping(peer_pub, peer_bytes.as_mut_ptr(), 65);
    }
    let mut fb = p256::FieldBytes::default();
    fb.copy_from_slice(&priv_bytes);
    let sk = match p256::SecretKey::from_bytes(&fb) {
        Ok(sk) => sk,
        Err(_) => return CRYPT_FAIL,
    };
    let pk = match p256::PublicKey::from_sec1_bytes(&peer_bytes) {
        Ok(pk) => pk,
        Err(_) => return CRYPT_FAIL,
    };
    let shared = p256::ecdh::diffie_hellman(sk.to_nonzero_scalar(), pk.as_affine());
    unsafe {
        core::ptr::copy_nonoverlapping(shared.raw_secret_bytes().as_slice().as_ptr(), shared_out, 32);
    }
    0
}
