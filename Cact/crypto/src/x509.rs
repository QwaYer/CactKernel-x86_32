//! X.509 chain verification and TLS handshake-signature checking.
//!
//! Built on the vendored rustls-webpki, with the cryptographic half supplied by
//! [`crate::sig`] — this is what makes "https" mean something again: a
//! certificate now has to chain to a trusted root, match the requested
//! hostname, be within its validity window, and prove possession of its key by
//! signing the handshake transcript.
//!
//! Trust anchors are *caller-provided* (the OS ships them as a file, not
//! compiled into the kernel), so the kernel holds no trust policy of its own.
//!
//! Deliberately not supported yet: CRLs/OCSP (revocation is skipped, which is
//! what most clients do by default) and client certificates.

use alloc::vec::Vec;
use core::time::Duration;

use rustls::crypto::WebPkiSupportedAlgorithms;
use rustls::pki_types::{
    alg_id, AlgorithmIdentifier, CertificateDer, InvalidSignature, ServerName,
    SignatureVerificationAlgorithm, UnixTime,
};
use rustls::SignatureScheme;

use crate::sig;

// ────────────────────────── verification algorithms ────────────────────────

/// A `SignatureVerificationAlgorithm` backed by [`crate::sig`]: the identifiers
/// webpki matches on, plus our own scheme selector for the actual math.
#[derive(Debug)]
struct CactAlgorithm {
    public_key_alg_id: AlgorithmIdentifier,
    signature_alg_id: AlgorithmIdentifier,
    scheme: u32,
}

impl SignatureVerificationAlgorithm for CactAlgorithm {
    fn public_key_alg_id(&self) -> AlgorithmIdentifier {
        self.public_key_alg_id
    }

    fn signature_alg_id(&self) -> AlgorithmIdentifier {
        self.signature_alg_id
    }

    fn verify_signature(
        &self,
        public_key: &[u8],
        message: &[u8],
        signature: &[u8],
    ) -> Result<(), InvalidSignature> {
        if sig::verify(self.scheme, public_key, message, signature) {
            Ok(())
        } else {
            Err(InvalidSignature)
        }
    }
}

pub static ECDSA_P256_SHA256: &dyn SignatureVerificationAlgorithm = &CactAlgorithm {
    public_key_alg_id: alg_id::ECDSA_P256,
    signature_alg_id: alg_id::ECDSA_SHA256,
    scheme: sig::SIG_ECDSA_P256_SHA256,
};

pub static ECDSA_P384_SHA384: &dyn SignatureVerificationAlgorithm = &CactAlgorithm {
    public_key_alg_id: alg_id::ECDSA_P384,
    signature_alg_id: alg_id::ECDSA_SHA384,
    scheme: sig::SIG_ECDSA_P384_SHA384,
};

pub static RSA_PKCS1_SHA256: &dyn SignatureVerificationAlgorithm = &CactAlgorithm {
    public_key_alg_id: alg_id::RSA_ENCRYPTION,
    signature_alg_id: alg_id::RSA_PKCS1_SHA256,
    scheme: sig::SIG_RSA_PKCS1_SHA256,
};

pub static RSA_PKCS1_SHA384: &dyn SignatureVerificationAlgorithm = &CactAlgorithm {
    public_key_alg_id: alg_id::RSA_ENCRYPTION,
    signature_alg_id: alg_id::RSA_PKCS1_SHA384,
    scheme: sig::SIG_RSA_PKCS1_SHA384,
};

pub static RSA_PKCS1_SHA512: &dyn SignatureVerificationAlgorithm = &CactAlgorithm {
    public_key_alg_id: alg_id::RSA_ENCRYPTION,
    signature_alg_id: alg_id::RSA_PKCS1_SHA512,
    scheme: sig::SIG_RSA_PKCS1_SHA512,
};

pub static RSA_PSS_SHA256: &dyn SignatureVerificationAlgorithm = &CactAlgorithm {
    public_key_alg_id: alg_id::RSA_ENCRYPTION,
    signature_alg_id: alg_id::RSA_PSS_SHA256,
    scheme: sig::SIG_RSA_PSS_SHA256,
};

pub static RSA_PSS_SHA384: &dyn SignatureVerificationAlgorithm = &CactAlgorithm {
    public_key_alg_id: alg_id::RSA_ENCRYPTION,
    signature_alg_id: alg_id::RSA_PSS_SHA384,
    scheme: sig::SIG_RSA_PSS_SHA384,
};

pub static RSA_PSS_SHA512: &dyn SignatureVerificationAlgorithm = &CactAlgorithm {
    public_key_alg_id: alg_id::RSA_ENCRYPTION,
    signature_alg_id: alg_id::RSA_PSS_SHA512,
    scheme: sig::SIG_RSA_PSS_SHA512,
};

/// Every algorithm we can verify — this is what webpki walks when checking the
/// signatures inside a chain (certificates sign each other with PKCS#1 v1.5 as
/// often as with PSS, so both families have to be listed).
pub static ALL_ALGORITHMS: &[&dyn SignatureVerificationAlgorithm] = &[
    ECDSA_P256_SHA256,
    ECDSA_P384_SHA384,
    RSA_PKCS1_SHA256,
    RSA_PKCS1_SHA384,
    RSA_PKCS1_SHA512,
    RSA_PSS_SHA256,
    RSA_PSS_SHA384,
    RSA_PSS_SHA512,
];

/// Algorithms plus the TLS `SignatureScheme` mapping.  Order matters: the
/// mapping is what gets advertised, most preferred first.
pub static SUPPORTED_ALGORITHMS: WebPkiSupportedAlgorithms = WebPkiSupportedAlgorithms {
    all: ALL_ALGORITHMS,
    mapping: &[
        (
            SignatureScheme::ECDSA_NISTP256_SHA256,
            &[ECDSA_P256_SHA256],
        ),
        (
            SignatureScheme::ECDSA_NISTP384_SHA384,
            &[ECDSA_P384_SHA384],
        ),
        (SignatureScheme::RSA_PSS_SHA256, &[RSA_PSS_SHA256]),
        (SignatureScheme::RSA_PSS_SHA384, &[RSA_PSS_SHA384]),
        (SignatureScheme::RSA_PSS_SHA512, &[RSA_PSS_SHA512]),
        (SignatureScheme::RSA_PKCS1_SHA256, &[RSA_PKCS1_SHA256]),
        (SignatureScheme::RSA_PKCS1_SHA384, &[RSA_PKCS1_SHA384]),
        (SignatureScheme::RSA_PKCS1_SHA512, &[RSA_PKCS1_SHA512]),
    ],
};

/// TLS `SignatureScheme` code (as it appears on the wire) → our algorithm.
/// Written out longhand rather than via `From<u16>` so the mapping is visible
/// and unknown codes are refused instead of silently becoming `Unknown`.
pub fn algorithm_for_tls_code(code: u16) -> Option<&'static dyn SignatureVerificationAlgorithm> {
    let scheme = match code {
        0x0401 => SignatureScheme::RSA_PKCS1_SHA256,
        0x0501 => SignatureScheme::RSA_PKCS1_SHA384,
        0x0601 => SignatureScheme::RSA_PKCS1_SHA512,
        0x0403 => SignatureScheme::ECDSA_NISTP256_SHA256,
        0x0503 => SignatureScheme::ECDSA_NISTP384_SHA384,
        0x0804 => SignatureScheme::RSA_PSS_SHA256,
        0x0805 => SignatureScheme::RSA_PSS_SHA384,
        0x0806 => SignatureScheme::RSA_PSS_SHA512,
        _ => return None,
    };
    SUPPORTED_ALGORITHMS
        .mapping
        .iter()
        .find(|(s, _)| *s == scheme)
        .and_then(|(_, algs)| algs.first().copied())
}

// ───────────────────────────── chain verification ──────────────────────────

/// Decode a DER length field.  Returns `(content_len, length_field_len)` — the
/// header size *excluding* the tag byte, which the caller counts itself.
fn der_length(bytes: &[u8]) -> Option<(usize, usize)> {
    let first = *bytes.first()?;
    if first < 0x80 {
        return Some((first as usize, 1));
    }
    let n = (first & 0x7f) as usize;
    if n == 0 || n > 4 || bytes.len() < 1 + n {
        return None;
    }
    let mut len = 0usize;
    for i in 0..n {
        len = (len << 8) | bytes[1 + i] as usize;
    }
    Some((len, 1 + n))
}

/// Split a buffer of concatenated DER certificates into its members.
///
/// Each certificate is a SEQUENCE, whose header carries its own length, so the
/// walk is unambiguous.  A malformed buffer yields no certificates — callers
/// treat that as a failed verification, never as a partial success.
fn der_certificates(buf: &[u8]) -> Option<Vec<&[u8]>> {
    let mut out = Vec::new();
    let mut off = 0usize;
    while off < buf.len() {
        if buf[off] != 0x30 {
            return None;
        }
        let (len, len_field) = der_length(&buf[off + 1..])?;
        // tag byte + length field + content
        let total = 1 + len_field + len;
        if len == 0 || off + total > buf.len() {
            return None;
        }
        out.push(&buf[off..off + total]);
        off += total;
    }
    if out.is_empty() {
        None
    } else {
        Some(out)
    }
}

/// Outcome of a chain check: `Ok(())` means the chain is valid for the
/// hostname at the given time and (when supplied) the handshake signature was
/// made by the leaf key.
// A pass/fail verdict is the whole contract here; the single caller maps it to
// the C ABI's 0/-1, so there is nothing for a richer error type to carry.
#[allow(clippy::result_unit_err)]
pub fn verify_chain(
    chain_der: &[u8],
    roots_der: &[u8],
    hostname: &str,
    unix_time: u64,
    handshake: Option<(u16, &[u8], &[u8])>,
) -> Result<(), ()> {
    let chain = der_certificates(chain_der).ok_or(())?;
    let roots = der_certificates(roots_der).ok_or(())?;

    let leaf = CertificateDer::from(chain[0]);
    let intermediates: Vec<CertificateDer<'_>> =
        chain[1..].iter().map(|c| CertificateDer::from(*c)).collect();

    // Anchors borrow from `roots`, so `roots` has to outlive them.
    let root_certs: Vec<CertificateDer<'_>> = roots.iter().map(|r| CertificateDer::from(*r)).collect();
    let mut anchors = Vec::with_capacity(root_certs.len());
    for rc in &root_certs {
        anchors.push(webpki::anchor_from_trusted_cert(rc).map_err(|_| ())?);
    }

    let cert = webpki::EndEntityCert::try_from(&leaf).map_err(|_| ())?;
    let time = UnixTime::since_unix_epoch(Duration::from_secs(unix_time));

    cert.verify_for_usage(
        SUPPORTED_ALGORITHMS.all,
        &anchors,
        &intermediates,
        time,
        webpki::KeyUsage::server_auth(),
        None, // no revocation lists
        None, // no path observer
    )
    .map_err(|_| ())?;

    let name = ServerName::try_from(hostname).map_err(|_| ())?;
    cert.verify_is_valid_for_subject_name(&name).map_err(|_| ())?;

    if let Some((scheme_code, msg, signature)) = handshake {
        let alg = algorithm_for_tls_code(scheme_code).ok_or(())?;
        cert.verify_signature(alg, msg, signature).map_err(|_| ())?;
    }

    Ok(())
}

// ──────────────────────────────── C ABI ────────────────────────────────────

unsafe fn cslice<'a>(p: *const u8, len: u32) -> Option<&'a [u8]> {
    if p.is_null() {
        if len == 0 {
            return Some(&[]);
        }
        return None;
    }
    // SAFETY: `p` is non-null and the caller guarantees `len` readable bytes at
    // `p` that outlive the returned reference.
    Some(unsafe { core::slice::from_raw_parts(p, len as usize) })
}

/// C ABI for `/dev/crypto`.  All pointers must be kernel addresses: the ioctl
/// handler copies the user buffers in first.
///
/// `tls_scheme == 0` skips the handshake-signature check; otherwise the
/// signature over `hs_msg` must verify against the leaf certificate's key.
///
/// Returns 0 when the chain is valid, -1 when it is not, -22 on malformed
/// input.
///
/// # Safety
///
/// `chain`, `roots`, `hs_msg` and `hs_sig` must each be null or point to at
/// least `chain_len`, `roots_len`, `hs_msg_len` and `hs_sig_len` readable bytes
/// respectively for the duration of the call; `hostname` must be null or point
/// to a NUL-terminated byte sequence.
#[no_mangle]
pub unsafe extern "C" fn cact_x509_verify(
    chain: *const u8,
    chain_len: u32,
    roots: *const u8,
    roots_len: u32,
    hostname: *const core::ffi::c_char,
    unix_time: u64,
    tls_scheme: u32,
    hs_msg: *const u8,
    hs_msg_len: u32,
    hs_sig: *const u8,
    hs_sig_len: u32,
) -> i32 {
    // SAFETY: `chain` is null or points to `chain_len` readable bytes (caller contract).
    let chain = unsafe { cslice(chain, chain_len) };
    // SAFETY: `roots` is null or points to `roots_len` readable bytes (caller contract).
    let roots = unsafe { cslice(roots, roots_len) };
    // SAFETY: `hs_msg` is null or points to `hs_msg_len` readable bytes (caller contract).
    let msg = unsafe { cslice(hs_msg, hs_msg_len) };
    // SAFETY: `hs_sig` is null or points to `hs_sig_len` readable bytes (caller contract).
    let signature = unsafe { cslice(hs_sig, hs_sig_len) };
    let (Some(chain), Some(roots), Some(msg), Some(signature)) =
        (chain, roots, msg, signature)
    else {
        return -22;
    };
    if hostname.is_null() {
        return -22;
    }
    // SAFETY: hostname is NUL-terminated and bounded (checked by the caller).
    let Some(name) = (unsafe { cstr(hostname) }) else {
        return -22;
    };
    let handshake = if tls_scheme == 0 {
        None
    } else if tls_scheme <= u16::MAX as u32 {
        Some((tls_scheme as u16, msg, signature))
    } else {
        return -22;
    };

    match verify_chain(chain, roots, &name, unix_time, handshake) {
        Ok(()) => 0,
        Err(()) => -1,
    }
}

/// Read a NUL-terminated C string, bounded to keep a hostile input from
/// scanning kernel memory forever.
unsafe fn cstr(ptr: *const core::ffi::c_char) -> Option<alloc::string::String> {
    let p = ptr as *const u8;
    let mut len = 0usize;
    loop {
        // SAFETY: the caller guarantees `ptr` points into a readable allocation
        // holding a NUL-terminated string, so `p.add(len)` stays in bounds.
        let cur = unsafe { p.add(len) };
        // SAFETY: `cur` is in bounds of that readable allocation, so reading the
        // byte at it is valid.
        if unsafe { *cur } == 0 {
            break;
        }
        len += 1;
        if len > 255 {
            return None;
        }
    }
    // SAFETY: the loop stopped at the NUL terminator, so `p` is valid for `len`
    // initialised bytes and the slice contains no interior NUL.
    let slice = unsafe { core::slice::from_raw_parts(p, len) };
    core::str::from_utf8(slice).ok().map(alloc::string::String::from)
}
