//! CCMP (AES-CCM) for WPA2 data frames, in software — the mac80211-compatible
//! profile.
//!
//! Ported from the RT2800USB module's `rt2800_ccmp.c` *after* the two layout
//! fixes of 2026-10-09 (on-air CCMP header and the tag's counter block), and
//! kept byte-for-byte interoperable with mac80211/OpenSSL CCM:
//!
//!   * on-air CCMP header: PN0, PN1, reserved, key id|ExtIV, PN2..PN5
//!     (most-significant byte first);
//!   * MIC = CBC-MAC over (AAD, payload), XORed with the CTR keystream block at
//!     counter 0; the payload uses counters 1..n (RFC 3610 / NIST 800-38C).
//!
//! The chip's hardware crypto is not used, so the driver must mark frames
//! unencrypted (TXWI WIV=1).

use crate::aes::Aes128;

fn xor16(d: &mut [u8; 16], s: &[u8; 16]) {
    for i in 0..16 {
        d[i] ^= s[i];
    }
}

/// mac80211's `ccmp_gcmp_aad()`: the data header with the subtype bits and
/// Retry/PwrMgmt/MoreData (and Order, for QoS) masked to 0, Protected set to 1,
/// the sequence number cleared, and the QoS control reduced to its TID.
fn aad(hdr: &[u8], hdr_len: usize, out: &mut [u8; 24]) -> usize {
    let qos = hdr_len >= 26;
    let mut fc1 = (hdr[1] & 0xc7) | 0x40;
    if qos {
        fc1 &= !0x80;
    }
    out[0] = hdr[0] & 0x8f;
    out[1] = fc1;
    out[2..20].copy_from_slice(&hdr[4..22]); // A1, A2, A3
    out[20] = hdr[22] & 0x0f; // SC fragment
    out[21] = 0x00; // sequence number cleared
    if qos {
        out[22] = hdr[24] & 0x0f; // QoS control: TID only
        out[23] = 0x00;
        24
    } else {
        22
    }
}

/// The CCMP nonce: Nonce Flags | A2 | PN (13 bytes).
fn nonce(hdr: &[u8], hdr_len: usize, pn: u64, out: &mut [u8; 13]) {
    out[0] = if hdr_len >= 26 { hdr[24] & 0x0f } else { 0 };
    out[1..7].copy_from_slice(&hdr[10..16]); // A2
    for i in 0..6 {
        out[7 + i] = (pn >> (8 * i)) as u8;
    }
}

fn mic(aes: &Aes128, nonce: &[u8; 13], aad: &[u8], payload: &[u8]) -> [u8; 8] {
    let mut b0 = [0u8; 16];
    b0[0] = 0x59; // Adata=1, M=8, L=2
    b0[1..14].copy_from_slice(nonce);
    b0[14] = (payload.len() >> 8) as u8;
    b0[15] = payload.len() as u8;
    let mut x = b0;
    aes.encrypt_block(&mut x);

    let mut blk = [0u8; 16];
    blk[0] = (aad.len() >> 8) as u8;
    blk[1] = aad.len() as u8;
    let mut bi = 2usize;
    // AAD blocks
    for &byte in aad {
        blk[bi] = byte;
        bi += 1;
        if bi == 16 {
            xor16(&mut x, &blk);
            aes.encrypt_block(&mut x);
            blk = [0u8; 16];
            bi = 0;
        }
    }
    // The AAD-length prefix shares the first block with the first AAD bytes, so
    // the partially filled block is flushed once after the AAD (even if empty,
    // padding it with zeros).
    if bi != 0 {
        xor16(&mut x, &blk);
        aes.encrypt_block(&mut x);
    }
    // Payload blocks start fresh.
    blk = [0u8; 16];
    bi = 0;
    for &byte in payload {
        blk[bi] = byte;
        bi += 1;
        if bi == 16 {
            xor16(&mut x, &blk);
            aes.encrypt_block(&mut x);
            blk = [0u8; 16];
            bi = 0;
        }
    }
    if bi != 0 {
        xor16(&mut x, &blk);
        aes.encrypt_block(&mut x);
    }

    let mut tag = [0u8; 8];
    tag.copy_from_slice(&x[..8]);
    tag
}

/// CTR keystream: XOR `data` with the AES keystream starting at `counter`.
fn ctr(aes: &Aes128, nonce: &[u8; 13], data: &mut [u8], counter_start: u16) {
    let mut counter = counter_start;
    let mut off = 0;
    while off < data.len() {
        let mut a = [0u8; 16];
        a[0] = 0x01; // L-1 = 1
        a[1..14].copy_from_slice(nonce);
        a[14] = (counter >> 8) as u8;
        a[15] = counter as u8;
        let mut s = a;
        aes.encrypt_block(&mut s);
        let n = core::cmp::min(16, data.len() - off);
        for i in 0..n {
            data[off + i] ^= s[i];
        }
        off += 16;
        counter = counter.wrapping_add(1);
    }
}

/// Encrypt a data frame into `out` as
/// `MAC header | CCMP header | ciphertext | MIC`.  `pn` must be unique per key.
/// Returns the total length written.
pub fn encrypt(
    tk: &[u8; 16],
    key_id: u8,
    pn: u64,
    hdr: &[u8],
    hdr_len: usize,
    payload: &[u8],
    out: &mut [u8],
) -> Option<usize> {
    let total = hdr_len + 8 + payload.len() + 8;
    if out.len() < total {
        return None;
    }
    let aes = Aes128::new(tk);

    let mut a = [0u8; 24];
    let aad_len = aad(hdr, hdr_len, &mut a);
    let mut nc = [0u8; 13];
    nonce(hdr, hdr_len, pn, &mut nc);
    let tag = mic(&aes, &nc, &a[..aad_len], payload);

    out[..hdr_len].copy_from_slice(&hdr[..hdr_len]);
    let ccmp = hdr_len;
    // PN0, PN1, reserved, key id|ExtIV, PN2..PN5 (most significant first).
    out[ccmp] = (pn >> 40) as u8;
    out[ccmp + 1] = (pn >> 32) as u8;
    out[ccmp + 2] = 0x00;
    out[ccmp + 3] = (key_id << 6) | 0x20;
    out[ccmp + 4] = (pn >> 24) as u8;
    out[ccmp + 5] = (pn >> 16) as u8;
    out[ccmp + 6] = (pn >> 8) as u8;
    out[ccmp + 7] = pn as u8;

    let ct = ccmp + 8;
    out[ct..ct + payload.len()].copy_from_slice(payload);
    ctr(&aes, &nc, &mut out[ct..ct + payload.len()], 1);

    // The tag is XORed with the counter-0 keystream block.
    let mut tag_ctr = tag;
    ctr(&aes, &nc, &mut tag_ctr, 0);
    out[ct + payload.len()..ct + payload.len() + 8].copy_from_slice(&tag_ctr);

    Some(total)
}

/// Decrypt in place: `f = MAC header | CCMP header | ciphertext | MIC`.
/// Returns the payload length, or None on a MIC failure.
pub fn decrypt(tk: &[u8; 16], f: &mut [u8], flen: usize, hdr_len: usize) -> Option<usize> {
    if flen < hdr_len + 8 + 8 {
        return None;
    }
    let aes = Aes128::new(tk);

    let ccmp = hdr_len;
    let pn = ((f[ccmp] as u64) << 40)
        | ((f[ccmp + 1] as u64) << 32)
        | ((f[ccmp + 4] as u64) << 24)
        | ((f[ccmp + 5] as u64) << 16)
        | ((f[ccmp + 6] as u64) << 8)
        | (f[ccmp + 7] as u64);

    let plen = flen - hdr_len - 8 - 8;
    if plen == 0 {
        return None;
    }

    let mut a = [0u8; 24];
    let aad_len = aad(f, hdr_len, &mut a);
    let mut nc = [0u8; 13];
    nonce(f, hdr_len, pn, &mut nc);

    let ct = ccmp + 8;
    // Recover the tag with the counter-0 keystream, then decrypt the payload.
    let mut m = [0u8; 8];
    m.copy_from_slice(&f[ct + plen..ct + plen + 8]);
    ctr(&aes, &nc, &mut m, 0);

    ctr(&aes, &nc, &mut f[ct..ct + plen], 1);

    let calc = mic(&aes, &nc, &a[..aad_len], &f[ct..ct + plen]);
    if calc != m {
        return None;
    }

    // Move the plaintext over the CCMP header.
    for i in 0..plen {
        f[hdr_len + i] = f[ct + i];
    }
    Some(plen)
}

/// Known-answer test from OpenSSL `EVP_aes_128_ccm` (with the mac80211 AAD and
/// the standard CCMP header), checked both ways.  Only an outside vector can
/// catch an on-air layout regression — a self-consistent round-trip cannot.
pub fn selftest() -> bool {
    let tk = [
        0xc9, 0x7c, 0x1f, 0x67, 0xce, 0x37, 0x11, 0x85, 0x51, 0x4a, 0x8a, 0x19, 0xf2, 0xbd, 0xd8,
        0x2f,
    ];
    let hdr = [
        0x08, 0x01, 0x00, 0x00, 0x00, 0x1c, 0xf0, 0xd0, 0x2c, 0x20, 0x44, 0x33, 0x4c, 0x46, 0x0b,
        0xec, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00,
    ];
    let payload = [
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e,
        0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d,
        0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    ];
    let frame = [
        0x08, 0x01, 0x00, 0x00, 0x00, 0x1c, 0xf0, 0xd0, 0x2c, 0x20, 0x44, 0x33, 0x4c, 0x46, 0x0b,
        0xec, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00,
        0x00, 0x01, 0x79, 0x67, 0xf4, 0x1c, 0x59, 0xc0, 0xfc, 0xdc, 0x82, 0x31, 0x1d, 0x30, 0xc1,
        0xb4, 0x9f, 0x03, 0x53, 0xac, 0xdd, 0xe5, 0xea, 0x7e, 0xab, 0xe4, 0x88, 0x0c, 0xe2, 0xd1,
        0x6e, 0x73, 0x32, 0x3d, 0x9d, 0x8a, 0xb5, 0xc8, 0x83, 0xb1, 0x3f, 0xc3, 0x26, 0xd9, 0x75,
        0x11, 0xd0, 0x16, 0x95, 0x37,
    ];

    let mut out = [0u8; 96];
    let n = match encrypt(&tk, 0, 1, &hdr, 24, &payload, &mut out) {
        Some(n) => n,
        None => return false,
    };
    if n != 80 || out[..80] != frame[..] {
        return false;
    }
    let mut back = [0u8; 80];
    back.copy_from_slice(&frame);
    match decrypt(&tk, &mut back, 80, 24) {
        Some(40) => back[24..64] == payload[..],
        _ => false,
    }
}
