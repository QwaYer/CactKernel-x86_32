use core::fmt::{self, Debug};

use rustls::crypto::SecureRandom;

pub struct CactRandom;

impl SecureRandom for CactRandom {
    fn fill(&self, buf: &mut [u8]) -> Result<(), rustls::crypto::GetRandomFailed> {
        #[cfg(target_arch = "x86")]
        unsafe {
            for chunk in buf.chunks_mut(4) {
                let mut val: u32 = 0;
                let ret = core::arch::x86::_rdrand32_step(&mut val);
                if ret == 0 {
                    return Err(rustls::crypto::GetRandomFailed);
                }
                let len = chunk.len().min(4);
                chunk.copy_from_slice(&val.to_le_bytes()[..len]);
            }
            Ok(())
        }
        #[cfg(not(target_arch = "x86"))]
        Err(rustls::crypto::GetRandomFailed)
    }
}

impl Debug for CactRandom {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "CactRandom")
    }
}

// ---- Deterministic RNG for x25519-dalek / p256 (insecure placeholder) ----
//
// WARNING: This is a simple LFSR-based generator used only because
// x25519-dalek requires a `CryptoRng` for key generation. It is NOT
// cryptographically secure. The seed is initialized from RDRAND when
// available. Replace with a proper kernel CSPRNG before production use.

static mut LFSR_STATE: u32 = 0;

fn lfsr32() -> u32 {
    unsafe {
        LFSR_STATE = LFSR_STATE.wrapping_add(1);
        let bit = ((LFSR_STATE >> 0) ^ (LFSR_STATE >> 2) ^ (LFSR_STATE >> 6) ^ (LFSR_STATE >> 31)) & 1;
        LFSR_STATE = (LFSR_STATE >> 1) | (bit << 31);
        LFSR_STATE
    }
}

/// Seed the LFSR from RDRAND at most once.
fn maybe_seed() {
    static mut SEEDED: bool = false;
    unsafe {
        if !SEEDED {
            #[cfg(target_arch = "x86")]
            {
                let mut seed: u32 = 0;
                let ret = core::arch::x86::_rdrand32_step(&mut seed);
                if ret != 0 {
                    LFSR_STATE = seed;
                } else {
                    LFSR_STATE = 0xABCD_1234;
                }
            }
            #[cfg(not(target_arch = "x86"))]
            {
                LFSR_STATE = 0xABCD_1234;
            }
            SEEDED = true;
        }
    }
}

pub struct CactRng;

impl rand_core::RngCore for CactRng {
    fn next_u32(&mut self) -> u32 {
        maybe_seed();
        lfsr32()
    }
    fn next_u64(&mut self) -> u64 {
        maybe_seed();
        (lfsr32() as u64) | ((lfsr32() as u64) << 32)
    }
    fn fill_bytes(&mut self, dest: &mut [u8]) {
        maybe_seed();
        for chunk in dest.chunks_mut(4) {
            let v = lfsr32().to_le_bytes();
            let len = chunk.len().min(4);
            chunk[..len].copy_from_slice(&v[..len]);
        }
    }

    fn try_fill_bytes(&mut self, dest: &mut [u8]) -> Result<(), rand_core::Error> {
        self.fill_bytes(dest);
        Ok(())
    }
}

// x25519-dalek and p256 require CryptoRng; we provide it despite the
// deterministic backing. See the warning above.
impl rand_core::CryptoRng for CactRng {}
