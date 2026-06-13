use alloc::vec;

use rustls::crypto::CryptoProvider;
use rustls::{CipherSuiteCommon, SupportedCipherSuite, Tls13CipherSuite};
use rustls::CipherSuite;
use rustls::crypto::WebPkiSupportedAlgorithms;

use crate::aead::{CactAes128Gcm, CactAes256Gcm};
use crate::hash::{CactSha256, CactSha384};
use crate::hkdf::{CactHkdfSha256, CactHkdfSha384};
use crate::kx::{CactX25519, CactSecp256r1};
use crate::random::CactRandom;
use crate::signer::CactKeyProvider;

pub fn cact_crypto_provider() -> CryptoProvider {
    CryptoProvider {
        cipher_suites: vec![
            SupportedCipherSuite::Tls13(&Tls13CipherSuite {
                common: CipherSuiteCommon {
                    suite: CipherSuite::TLS13_AES_128_GCM_SHA256,
                    hash_provider: &CactSha256,
                    confidentiality_limit: 1 << 23,
                },
                hkdf_provider: &CactHkdfSha256,
                aead_alg: &CactAes128Gcm,
                quic: None,
            }),
            SupportedCipherSuite::Tls13(&Tls13CipherSuite {
                common: CipherSuiteCommon {
                    suite: CipherSuite::TLS13_AES_256_GCM_SHA384,
                    hash_provider: &CactSha384,
                    confidentiality_limit: 1 << 23,
                },
                hkdf_provider: &CactHkdfSha384,
                aead_alg: &CactAes256Gcm,
                quic: None,
            }),
        ],
        kx_groups: vec![&CactX25519, &CactSecp256r1],
        signature_verification_algorithms: WebPkiSupportedAlgorithms {
            all: &[],
            mapping: &[],
        },
        secure_random: &CactRandom,
        key_provider: &CactKeyProvider,
    }
}
