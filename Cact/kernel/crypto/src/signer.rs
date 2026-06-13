use core::fmt::{self, Debug};
use alloc::sync::Arc;

use rustls::sign::SigningKey;
use rustls::pki_types::PrivateKeyDer;
use rustls::Error;

use rustls::crypto::KeyProvider;

pub struct CactKeyProvider;

impl KeyProvider for CactKeyProvider {
    fn load_private_key(&self, _key_der: PrivateKeyDer<'static>) -> Result<Arc<dyn SigningKey>, Error> {
        Err(Error::General("CactKeyProvider: key loading not implemented".into()))
    }
}

impl Debug for CactKeyProvider {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "CactKeyProvider")
    }
}
