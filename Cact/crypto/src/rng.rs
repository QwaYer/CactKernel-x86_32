//! Kernel CSPRNG — the single source of randomness for the whole system.
//!
//! A ChaCha20-based DRBG in the shape Linux uses for its CRNG: a 32-byte key
//! keys a ChaCha20 instance whose keystream is the output.  After every request
//! the key is replaced by fresh keystream (backtracking resistance) and the
//! state is stirred with whatever hardware entropy the CPU offers plus TSC
//! jitter, so a later state compromise does not expose earlier output.
//!
//! This replaces three separate non-sources the kernel used to have: an LCG
//! behind /dev/random and /dev/urandom, an LFSR seeded from one RDRAND value
//! that produced the TLS ephemeral keys, and bare RDRAND with no DRBG at all.
//!
//! Entropy: RDRAND is used when the CPU has it.  When it does not (many QEMU
//! CPU models, older parts), the pool falls back to boot-time TSC jitter, which
//! is the classic no-hardware-entropy source — weaker, and worth knowing about:
//! `cact_csprng_hw_entropy()` reports whether the hardware source was present.

use cact_shim::sync::Mutex;
use chacha20::cipher::{KeyIvInit, StreamCipher};
use chacha20::{ChaCha20, Key, Nonce};
use sha2::{Digest, Sha256};

// SAFETY: all three are C kernel symbols, linked into the final image.
unsafe extern "C" {
    // Reached only from the x86-32 `rdrand32`; other targets use a stub.
    #[cfg(target_arch = "x86")]
    fn cpu_has_rdrand() -> i32;
    fn ktime_get_usec() -> u64;
    fn timer_ticks_get() -> u32;
}


/// One hardware random word, or None when the CPU has no RDRAND.  RDRAND is
/// allowed to fail transiently, so retry a few times before giving up on it.
#[cfg(target_arch = "x86")]
fn rdrand32() -> Option<u32> {
    // SAFETY: guarded by the CPUID feature bit.
    if unsafe { cpu_has_rdrand() } == 0 {
        return None;
    }
    for _ in 0..10 {
        let mut v: u32 = 0;
        // SAFETY: RDRAND was confirmed present by the CPUID check above, and
        // `&mut v` is a valid place to write the instruction's result.
        let ok = unsafe { core::arch::x86::_rdrand32_step(&mut v) };
        if ok == 1 {
            return Some(v);
        }
    }
    None
}

#[cfg(not(target_arch = "x86"))]
fn rdrand32() -> Option<u32> {
    None
}

/// Cycle counter.  Its *low* bits are what carry jitter, so callers use deltas.
#[cfg(target_arch = "x86")]
fn rdtsc() -> u64 {
    let lo: u32;
    let hi: u32;
    // SAFETY: rdtsc is readable in ring 0.
    unsafe {
        core::arch::asm!("rdtsc", out("eax") lo, out("edx") hi,
                         options(nomem, nostack, preserves_flags));
    }
    ((hi as u64) << 32) | lo as u64
}

#[cfg(not(target_arch = "x86"))]
fn rdtsc() -> u64 {
    // SAFETY: pure C call; no TSC on other targets.
    unsafe { ktime_get_usec() }
}

/// Harvest timing jitter into `digest`.
///
/// The measured deltas are noisy because of interrupts, cache and pipeline
/// state; only the low bits are taken, the high bits are dominated by the
/// loop's own cost.  This is a fallback, not a substitute for RDRAND.
fn jitter(digest: &mut Sha256, samples: u32) {
    for _ in 0..samples {
        let t0 = rdtsc();
        let mut x = core::hint::black_box(t0);
        for _ in 0..16 {
            x = x.wrapping_mul(0x9E37_79B9_7F4A_7C15).rotate_left(17);
        }
        core::hint::black_box(x);
        let d = rdtsc().wrapping_sub(t0);
        digest.update((d as u32).to_le_bytes());
        digest.update(((d >> 32) as u32).to_le_bytes());
    }
}

struct Drbg {
    key: [u8; 32],
    nonce: [u8; 12],
    ready: bool,
    hw_entropy: bool,
}

static DRBG: Mutex<Drbg> = Mutex::new(Drbg {
    key: [0; 32],
    nonce: [0; 12],
    ready: false,
    hw_entropy: false,
});

/// Hash `domain` + `sources` (+ timing jitter) into 32 bytes.
///
/// Deliberately `#[inline(never)]`: a `Sha256` is a 32-byte state array, and
/// when this was inlined into `fill` the compiler placed a *live* spill of the
/// caller's output slice inside that array.  Keeping the hashers in their own
/// frames keeps the caller's frame free of arrays big enough to collide with
/// its own live values.
#[inline(never)]
fn hash_sources(domain: &[u8], sources: &[u8], jitter_samples: u32) -> [u8; 32] {
    let mut h = Sha256::new();
    h.update(domain);
    h.update(sources);
    if jitter_samples != 0 {
        jitter(&mut h, jitter_samples);
    }
    let mut out = [0u8; 32];
    out.copy_from_slice(&h.finalize());
    out
}

/// First seeding: mix every source available at boot into one key.
#[inline(never)]
fn seed(g: &mut Drbg) {
    let mut src = [0u8; 64];
    let mut n = 0usize;
    for _ in 0..8 {
        if let Some(r) = rdrand32() {
            src[n..n + 4].copy_from_slice(&r.to_le_bytes());
            n += 4;
            g.hw_entropy = true;
        }
    }
    // SAFETY: C kernel timer/clock.
    src[n..n + 8].copy_from_slice(&unsafe { ktime_get_usec() }.to_le_bytes());
    n += 8;
    // SAFETY: C kernel timer; no arguments, only reads the tick counter.
    src[n..n + 4].copy_from_slice(&unsafe { timer_ticks_get() }.to_le_bytes());
    n += 4;
    let stack_probe = 0u8;
    src[n..n + 4].copy_from_slice(&((&stack_probe as *const u8 as usize as u32).to_le_bytes()));
    n += 4;

    g.key = hash_sources(b"cact-csprng-v1-seed", &src[..n], 512);

    // Independent nonce material; the same hash would tie the two together.
    let mut n2 = [0u8; 48];
    n2[..32].copy_from_slice(&g.key);
    // SAFETY: C kernel timer/clock.
    n2[32..40].copy_from_slice(&unsafe { ktime_get_usec() }.to_le_bytes());
    // SAFETY: C kernel timer; no arguments, only reads the tick counter.
    n2[40..44].copy_from_slice(&unsafe { timer_ticks_get() }.to_le_bytes());
    g.nonce.copy_from_slice(&hash_sources(b"cact-csprng-v1-nonce", &n2[..44], 64)[..12]);
    g.ready = true;
}

/// Per-request stir.  XORing the digest in is safe even when nothing new is
/// available: it is a deterministic function of the current state then, so the
/// state is not weakened, only re-mixed.
#[inline(never)]
fn stir(g: &mut Drbg) {
    let mut src = [0u8; 64];
    src[..32].copy_from_slice(&g.key);
    let mut n = 32usize;
    for _ in 0..4 {
        if let Some(r) = rdrand32() {
            src[n..n + 4].copy_from_slice(&r.to_le_bytes());
            n += 4;
        }
    }
    // SAFETY: C kernel timer/clock.
    src[n..n + 8].copy_from_slice(&unsafe { ktime_get_usec() }.to_le_bytes());
    n += 8;
    // SAFETY: C kernel timer; no arguments, only reads the tick counter.
    src[n..n + 4].copy_from_slice(&unsafe { timer_ticks_get() }.to_le_bytes());
    n += 4;

    let d = hash_sources(b"cact-csprng-v1-stir", &src[..n], 32);
    for (k, x) in g.key.iter_mut().zip(d.iter()) {
        *k ^= *x;
    }
}

/// Advance the 96-bit nonce (little-endian counter).  Every request uses a fresh
/// nonce, so keystream blocks are never reused.
#[inline(never)]
fn bump(nonce: &mut [u8; 12]) {
    for b in nonce.iter_mut() {
        *b = b.wrapping_add(1);
        if *b != 0 {
            break;
        }
    }
}

/// Apply ChaCha20 keystream to `buf`, rekey from fresh keystream, then advance.
/// Out of line for the same reason as `hash_sources`: the cipher's own state is
/// a 64-byte array, and `fill` must not have it in the same frame as the slice
/// it is filling.
#[inline(never)]
fn apply_and_rekey(key: &mut [u8; 32], nonce: &mut [u8; 12], buf: &mut [u8]) {
    ChaCha20::new(Key::from_slice(key), Nonce::from_slice(nonce)).apply_keystream(buf);
    bump(nonce);

    let mut next_key = [0u8; 32];
    ChaCha20::new(Key::from_slice(key), Nonce::from_slice(nonce)).apply_keystream(&mut next_key);
    bump(nonce);

    key.copy_from_slice(&next_key);
}

/// Fill `buf` with cryptographically strong random bytes.
pub fn fill(buf: &mut [u8]) {
    if buf.is_empty() {
        return;
    }
    let mut g = DRBG.lock();
    if !g.ready {
        seed(&mut g);
    }
    stir(&mut g);

    // Borrow the state's two halves separately: the keystream step needs the
    // buffer mutably for the rest of the call.
    let Drbg { key, nonce, .. } = &mut *g;
    buf.fill(0);
    apply_and_rekey(key, nonce, buf);
}

/// C ABI: fill a kernel buffer.  Returns 0, or -1 on a null buffer.
///
/// # Safety
///
/// `buf` must point to at least `len` bytes of writable memory that stay valid
/// and unaliased for the duration of the call.
#[no_mangle]
pub unsafe extern "C" fn cact_csprng_fill(buf: *mut u8, len: u32) -> i32 {
    if len == 0 {
        return 0;
    }
    if buf.is_null() {
        return -1;
    }
    // SAFETY: the caller promises `len` writable bytes at `buf`.
    fill(unsafe { core::slice::from_raw_parts_mut(buf, len as usize) });
    0
}

/// Self-test: two draws must be non-zero, and different from each other.  Run
/// once at boot — a DRBG that silently hands out zeros is worse than none, and
/// this is cheap enough to check every time.
#[no_mangle]
pub extern "C" fn cact_csprng_selftest() -> i32 {
    let mut a = [0u8; 32];
    let mut b = [0u8; 32];
    fill(&mut a);
    fill(&mut b);
    let nz = a.iter().filter(|x| **x != 0).count() + b.iter().filter(|x| **x != 0).count();
    if nz == 0 || a == b {
        return -1;
    }
    0
}

/// C ABI: 1 when the CSPRNG was seeded from a hardware source (RDRAND), 0 when
/// it had to fall back to timing jitter alone.
#[no_mangle]
pub extern "C" fn cact_csprng_hw_entropy() -> i32 {
    let g = DRBG.lock();
    if (g.ready && g.hw_entropy) || rdrand32().is_some() {
        1
    } else {
        0
    }
}
