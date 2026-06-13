use core::fmt::{self, Debug};
use alloc::boxed::Box;

use elliptic_curve::sec1::ToEncodedPoint;
use rustls::crypto::{
    ActiveKeyExchange, SharedSecret, SupportedKxGroup,
};
use rustls::NamedGroup;
use rustls::Error;

pub struct CactX25519;
pub struct CactSecp256r1;

impl SupportedKxGroup for CactX25519 {
    fn start(&self) -> Result<Box<dyn ActiveKeyExchange>, Error> {
        Ok(Box::new(X25519Exchange::start()))
    }
    fn name(&self) -> NamedGroup { NamedGroup::X25519 }
}

impl SupportedKxGroup for CactSecp256r1 {
    fn start(&self) -> Result<Box<dyn ActiveKeyExchange>, Error> {
        Ok(Box::new(P256Exchange::start()))
    }
    fn name(&self) -> NamedGroup { NamedGroup::secp256r1 }
}

struct X25519Exchange {
    secret: x25519_dalek::EphemeralSecret,
    pub_key: [u8; 32],
}

impl X25519Exchange {
    fn start() -> Self {
        let secret = x25519_dalek::EphemeralSecret::random_from_rng(&mut crate::random::CactRng);
        let pub_key = x25519_dalek::PublicKey::from(&secret).to_bytes();
        X25519Exchange { secret, pub_key }
    }
}

impl ActiveKeyExchange for X25519Exchange {
    fn complete(self: Box<Self>, peer_pub_key: &[u8]) -> Result<SharedSecret, Error> {
        let mut arr = [0u8; 32];
        if peer_pub_key.len() != 32 {
            return Err(Error::General("invalid X25519 public key length".into()));
        }
        arr.copy_from_slice(peer_pub_key);
        let pub_point = x25519_dalek::PublicKey::from(arr);
        let shared = self.secret.diffie_hellman(&pub_point);
        Ok(shared.as_bytes().to_vec().into())
    }
    fn pub_key(&self) -> &[u8] { &self.pub_key }
    fn group(&self) -> NamedGroup { NamedGroup::X25519 }
}

struct P256Exchange {
    secret: p256::ecdh::EphemeralSecret,
    pub_key_encoded: [u8; 65],
}

impl P256Exchange {
    fn start() -> Self {
        let secret = p256::ecdh::EphemeralSecret::random(&mut crate::random::CactRng);
        let pub_key = p256::PublicKey::from(&secret);
        let ep = pub_key.to_encoded_point(false);
        let mut pub_key_encoded = [0u8; 65];
        pub_key_encoded.copy_from_slice(ep.as_bytes());
        P256Exchange { secret, pub_key_encoded }
    }
}

impl ActiveKeyExchange for P256Exchange {
    fn complete(self: Box<Self>, peer_pub_key: &[u8]) -> Result<SharedSecret, Error> {
        let peer = p256::PublicKey::from_sec1_bytes(peer_pub_key)
            .map_err(|_| Error::General("invalid P-256 public key".into()))?;
        let shared = self.secret.diffie_hellman(&peer);
        let raw = shared.raw_secret_bytes();
        Ok(raw.to_vec().into())
    }
    fn pub_key(&self) -> &[u8] {
        &self.pub_key_encoded
    }
    fn group(&self) -> NamedGroup { NamedGroup::secp256r1 }
}

impl Debug for CactX25519 {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result { write!(f, "CactX25519") }
}
impl Debug for CactSecp256r1 {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result { write!(f, "CactSecp256r1") }
}
