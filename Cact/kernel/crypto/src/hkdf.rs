use core::fmt::{self, Debug};
use alloc::boxed::Box;

use rustls::crypto::tls13::{Hkdf, HkdfExpander, OkmBlock, OutputLengthError};

pub struct CactHkdfSha256;
pub struct CactHkdfSha384;

struct CactHkdfExpanderSha256(hkdf::Hkdf<sha2::Sha256>);
struct CactHkdfExpanderSha384(hkdf::Hkdf<sha2::Sha384>);

impl Hkdf for CactHkdfSha256 {
    fn extract_from_zero_ikm(&self, salt: Option<&[u8]>) -> Box<dyn HkdfExpander> {
        let (_, hkdf) = hkdf::Hkdf::<sha2::Sha256>::extract(salt, &[]);
        Box::new(CactHkdfExpanderSha256(hkdf))
    }
    fn extract_from_secret(&self, salt: Option<&[u8]>, secret: &[u8]) -> Box<dyn HkdfExpander> {
        let (_, hkdf) = hkdf::Hkdf::<sha2::Sha256>::extract(salt, secret);
        Box::new(CactHkdfExpanderSha256(hkdf))
    }
    fn expander_for_okm(&self, okm: &OkmBlock) -> Box<dyn HkdfExpander> {
        let hkdf = hkdf::Hkdf::<sha2::Sha256>::from_prk(okm.as_ref())
            .expect("valid PRK");
        Box::new(CactHkdfExpanderSha256(hkdf))
    }
    fn hmac_sign(&self, key: &OkmBlock, message: &[u8]) -> rustls::crypto::hmac::Tag {
        use hmac::Mac;
        let mut mac = hmac::Hmac::<sha2::Sha256>::new_from_slice(key.as_ref())
            .expect("valid HMAC key");
        mac.update(message);
        rustls::crypto::hmac::Tag::new(&mac.finalize().into_bytes())
    }
}

impl Hkdf for CactHkdfSha384 {
    fn extract_from_zero_ikm(&self, salt: Option<&[u8]>) -> Box<dyn HkdfExpander> {
        let (_, hkdf) = hkdf::Hkdf::<sha2::Sha384>::extract(salt, &[]);
        Box::new(CactHkdfExpanderSha384(hkdf))
    }
    fn extract_from_secret(&self, salt: Option<&[u8]>, secret: &[u8]) -> Box<dyn HkdfExpander> {
        let (_, hkdf) = hkdf::Hkdf::<sha2::Sha384>::extract(salt, secret);
        Box::new(CactHkdfExpanderSha384(hkdf))
    }
    fn expander_for_okm(&self, okm: &OkmBlock) -> Box<dyn HkdfExpander> {
        let hkdf = hkdf::Hkdf::<sha2::Sha384>::from_prk(okm.as_ref())
            .expect("valid PRK");
        Box::new(CactHkdfExpanderSha384(hkdf))
    }
    fn hmac_sign(&self, key: &OkmBlock, message: &[u8]) -> rustls::crypto::hmac::Tag {
        use hmac::Mac;
        let mut mac = hmac::Hmac::<sha2::Sha384>::new_from_slice(key.as_ref())
            .expect("valid HMAC key");
        mac.update(message);
        rustls::crypto::hmac::Tag::new(&mac.finalize().into_bytes())
    }
}

impl HkdfExpander for CactHkdfExpanderSha256 {
    fn expand_slice(&self, info: &[&[u8]], output: &mut [u8]) -> Result<(), OutputLengthError> {
        self.0.expand_multi_info(info, output).map_err(|_| OutputLengthError)
    }
    fn expand_block(&self, info: &[&[u8]]) -> OkmBlock {
        let mut buf = [0u8; 32];
        self.0.expand_multi_info(info, &mut buf).ok();
        OkmBlock::new(&buf)
    }
    fn hash_len(&self) -> usize { 32 }
}

impl HkdfExpander for CactHkdfExpanderSha384 {
    fn expand_slice(&self, info: &[&[u8]], output: &mut [u8]) -> Result<(), OutputLengthError> {
        self.0.expand_multi_info(info, output).map_err(|_| OutputLengthError)
    }
    fn expand_block(&self, info: &[&[u8]]) -> OkmBlock {
        let mut buf = [0u8; 48];
        self.0.expand_multi_info(info, &mut buf).ok();
        OkmBlock::new(&buf)
    }
    fn hash_len(&self) -> usize { 48 }
}

impl Debug for CactHkdfSha256 {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result { write!(f, "CactHkdfSha256") }
}
impl Debug for CactHkdfSha384 {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result { write!(f, "CactHkdfSha384") }
}
