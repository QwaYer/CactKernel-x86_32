//! RNG adapters.  Both types draw from the kernel CSPRNG in [`crate::rng`];
//! there is no second generator hiding here any more.

use core::fmt::{self, Debug};

use rustls::crypto::SecureRandom;

/// rustls `SecureRandom`: nonces, HKDF salts and ephemeral keys.
pub struct CactRandom;

impl SecureRandom for CactRandom {
    fn fill(&self, buf: &mut [u8]) -> Result<(), rustls::crypto::GetRandomFailed> {
        crate::rng::fill(buf);
        Ok(())
    }
}

impl Debug for CactRandom {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "CactRandom")
    }
}

/// `rand_core` view of the same CSPRNG, for x25519-dalek / p256 key generation.
pub struct CactRng;

impl rand_core::RngCore for CactRng {
    fn next_u32(&mut self) -> u32 {
        let mut b = [0u8; 4];
        crate::rng::fill(&mut b);
        u32::from_le_bytes(b)
    }

    fn next_u64(&mut self) -> u64 {
        let mut b = [0u8; 8];
        crate::rng::fill(&mut b);
        u64::from_le_bytes(b)
    }

    fn fill_bytes(&mut self, dest: &mut [u8]) {
        crate::rng::fill(dest);
    }

    fn try_fill_bytes(&mut self, dest: &mut [u8]) -> Result<(), rand_core::Error> {
        crate::rng::fill(dest);
        Ok(())
    }
}

impl rand_core::CryptoRng for CactRng {}
