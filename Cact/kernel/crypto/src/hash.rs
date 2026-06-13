use alloc::boxed::Box;
use rustls::crypto::hash::{Context, Hash, Output};
use sha2::{Digest, Sha256, Sha384};

pub struct CactSha256;
pub struct CactSha384;

struct Sha256Ctx(sha2::Sha256);
struct Sha384Ctx(sha2::Sha384);

impl Hash for CactSha256 {
    fn start(&self) -> Box<dyn Context> {
        Box::new(Sha256Ctx(Sha256::new()))
    }
    fn hash(&self, data: &[u8]) -> Output {
        let mut h = Sha256::new();
        h.update(data);
        Output::new(&h.finalize())
    }
    fn output_len(&self) -> usize { 32 }
    fn algorithm(&self) -> rustls::crypto::hash::HashAlgorithm {
        rustls::crypto::hash::HashAlgorithm::SHA256
    }
}

impl Hash for CactSha384 {
    fn start(&self) -> Box<dyn Context> {
        Box::new(Sha384Ctx(Sha384::new()))
    }
    fn hash(&self, data: &[u8]) -> Output {
        let mut h = Sha384::new();
        h.update(data);
        Output::new(&h.finalize())
    }
    fn output_len(&self) -> usize { 48 }
    fn algorithm(&self) -> rustls::crypto::hash::HashAlgorithm {
        rustls::crypto::hash::HashAlgorithm::SHA384
    }
}

impl Context for Sha256Ctx {
    fn fork_finish(&self) -> Output {
        Output::new(&self.0.clone().finalize())
    }
    fn fork(&self) -> Box<dyn Context> {
        Box::new(Sha256Ctx(self.0.clone()))
    }
    fn finish(self: Box<Self>) -> Output {
        Output::new(&self.0.clone().finalize())
    }
    fn update(&mut self, data: &[u8]) { self.0.update(data); }
}

impl Context for Sha384Ctx {
    fn fork_finish(&self) -> Output {
        Output::new(&self.0.clone().finalize())
    }
    fn fork(&self) -> Box<dyn Context> {
        Box::new(Sha384Ctx(self.0.clone()))
    }
    fn finish(self: Box<Self>) -> Output {
        Output::new(&self.0.clone().finalize())
    }
    fn update(&mut self, data: &[u8]) { self.0.update(data); }
}
