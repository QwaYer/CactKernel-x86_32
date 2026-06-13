use core::fmt;
use alloc::boxed::Box;
use alloc::vec::Vec;

use aead::{AeadInPlace, KeyInit};
use aes_gcm::AesGcm;

use rustls::crypto::cipher::{
    AeadKey, Iv, Tls13AeadAlgorithm,
    MessageEncrypter, MessageDecrypter,
    OutboundPlainMessage, OutboundOpaqueMessage,
    InboundOpaqueMessage, InboundPlainMessage,
    PrefixedPayload,
};
use rustls::ContentType;
use rustls::ProtocolVersion;

pub struct CactAes128Gcm;
pub struct CactAes256Gcm;

const GCM_TAG_LEN: usize = 16;

fn tls13_nonce(iv: &[u8; 12], seq: u64) -> [u8; 12] {
    let mut nonce = *iv;
    for (i, b) in seq.to_be_bytes().iter().rev().enumerate() {
        nonce[11 - i] ^= b;
    }
    nonce
}

fn tls13_aad(plaintext_len: usize, seq: u64) -> [u8; 13] {
    let mut aad = [0u8; 13];
    aad[..8].copy_from_slice(&seq.to_be_bytes());
    aad[8..11].copy_from_slice(&[0x17, 0x03, 0x03]);
    aad[11..13].copy_from_slice(&(plaintext_len as u16).to_be_bytes());
    aad
}

struct Aes128GcmImpl { key: [u8; 16], iv: [u8; 12] }
struct Aes256GcmImpl { key: [u8; 32], iv: [u8; 12] }

impl Tls13AeadAlgorithm for CactAes128Gcm {
    fn encrypter(&self, key: AeadKey, iv: Iv) -> Box<dyn MessageEncrypter> {
        let k = <[u8; 16]>::try_from(key.as_ref()).unwrap_or([0u8; 16]);
        let i = <[u8; 12]>::try_from(iv.as_ref()).unwrap_or([0u8; 12]);
        Box::new(Aes128GcmImpl { key: k, iv: i })
    }
    fn decrypter(&self, key: AeadKey, iv: Iv) -> Box<dyn MessageDecrypter> {
        let k = <[u8; 16]>::try_from(key.as_ref()).unwrap_or([0u8; 16]);
        let i = <[u8; 12]>::try_from(iv.as_ref()).unwrap_or([0u8; 12]);
        Box::new(Aes128GcmImpl { key: k, iv: i })
    }
    fn key_len(&self) -> usize { 16 }
    fn extract_keys(&self, key: AeadKey, iv: Iv)
        -> Result<rustls::ConnectionTrafficSecrets, rustls::crypto::cipher::UnsupportedOperationError>
    {
        Ok(rustls::ConnectionTrafficSecrets::Aes128Gcm { key, iv })
    }
}

impl Tls13AeadAlgorithm for CactAes256Gcm {
    fn encrypter(&self, key: AeadKey, iv: Iv) -> Box<dyn MessageEncrypter> {
        let k = <[u8; 32]>::try_from(key.as_ref()).unwrap_or([0u8; 32]);
        let i = <[u8; 12]>::try_from(iv.as_ref()).unwrap_or([0u8; 12]);
        Box::new(Aes256GcmImpl { key: k, iv: i })
    }
    fn decrypter(&self, key: AeadKey, iv: Iv) -> Box<dyn MessageDecrypter> {
        let k = <[u8; 32]>::try_from(key.as_ref()).unwrap_or([0u8; 32]);
        let i = <[u8; 12]>::try_from(iv.as_ref()).unwrap_or([0u8; 12]);
        Box::new(Aes256GcmImpl { key: k, iv: i })
    }
    fn key_len(&self) -> usize { 32 }
    fn extract_keys(&self, key: AeadKey, iv: Iv)
        -> Result<rustls::ConnectionTrafficSecrets, rustls::crypto::cipher::UnsupportedOperationError>
    {
        Ok(rustls::ConnectionTrafficSecrets::Aes256Gcm { key, iv })
    }
}

impl MessageEncrypter for Aes128GcmImpl {
    fn encrypt(&mut self, msg: OutboundPlainMessage<'_>, seq: u64) -> Result<OutboundOpaqueMessage, rustls::Error> {
        let total_len = self.encrypted_payload_len(msg.payload.len());
        let nonce_block = tls13_nonce(&self.iv, seq);
        let aad = tls13_aad(total_len, seq);
        let cipher = AesGcm::<aes_gcm::aes::Aes128, typenum::U12>::new_from_slice(&self.key)
            .map_err(|_| rustls::Error::EncryptError)?;
        let mut payload = match msg.payload {
            rustls::crypto::cipher::OutboundChunks::Single(s) => s.to_vec(),
            rustls::crypto::cipher::OutboundChunks::Multiple { ref chunks, start, end } => {
                let mut v = Vec::new();
                for c in &chunks[start..end] { v.extend_from_slice(c); }
                v
            }
        };
        payload.push(u8::from(msg.typ));
        let nonce = aead::Nonce::<AesGcm<aes_gcm::aes::Aes128, typenum::U12>>::from_slice(&nonce_block);
        let tag = cipher.encrypt_in_place_detached(nonce, &aad, &mut payload)
            .map_err(|_| rustls::Error::EncryptError)?;
        let mut out_data = Vec::with_capacity(total_len);
        out_data.extend_from_slice(&payload);
        out_data.extend_from_slice(tag.as_slice());
        Ok(OutboundOpaqueMessage { typ: ContentType::ApplicationData, version: ProtocolVersion::TLSv1_2, payload: PrefixedPayload::from(out_data.as_slice()) })
    }
    fn encrypted_payload_len(&self, payload_len: usize) -> usize { payload_len + 1 + GCM_TAG_LEN }
}

impl MessageDecrypter for Aes128GcmImpl {
    fn decrypt<'a>(&mut self, mut msg: InboundOpaqueMessage<'a>, seq: u64) -> Result<InboundPlainMessage<'a>, rustls::Error> {
        if msg.payload.len() < 1 + GCM_TAG_LEN {
            return Err(rustls::Error::DecryptError);
        }

        let aad = tls13_aad(msg.payload.len(), seq);
        let nonce_block = tls13_nonce(&self.iv, seq);

        let cipher = AesGcm::<aes_gcm::aes::Aes128, typenum::U12>::new_from_slice(&self.key)
            .map_err(|_| rustls::Error::DecryptError)?;
        let nonce = aead::Nonce::<AesGcm<aes_gcm::aes::Aes128, typenum::U12>>::from_slice(&nonce_block);

        let ct_body_len = msg.payload.len() - 1 - GCM_TAG_LEN;
        let tag_bytes = msg.payload[msg.payload.len() - GCM_TAG_LEN..].to_vec();

        {
            let buf = &mut *msg.payload;
            let (_, ct_and_tag) = buf.split_at_mut(1);
            let (ciphertext, _tag_region) = ct_and_tag.split_at_mut(ct_body_len);
            let tag_slice = aead::Tag::<AesGcm<aes_gcm::aes::Aes128, typenum::U12>>::from_slice(&tag_bytes);
            cipher.decrypt_in_place_detached(nonce, &aad, ciphertext, tag_slice)
                .map_err(|_| rustls::Error::DecryptError)?;
        }

        msg.payload.truncate(1 + ct_body_len);
        msg.into_tls13_unpadded_message()
    }
}

impl MessageEncrypter for Aes256GcmImpl {
    fn encrypt(&mut self, msg: OutboundPlainMessage<'_>, seq: u64) -> Result<OutboundOpaqueMessage, rustls::Error> {
        let total_len = self.encrypted_payload_len(msg.payload.len());
        let nonce_block = tls13_nonce(&self.iv, seq);
        let aad = tls13_aad(total_len, seq);
        let cipher = AesGcm::<aes_gcm::aes::Aes256, typenum::U12>::new_from_slice(&self.key)
            .map_err(|_| rustls::Error::EncryptError)?;
        let mut payload = match msg.payload {
            rustls::crypto::cipher::OutboundChunks::Single(s) => s.to_vec(),
            rustls::crypto::cipher::OutboundChunks::Multiple { ref chunks, start, end } => {
                let mut v = Vec::new();
                for c in &chunks[start..end] { v.extend_from_slice(c); }
                v
            }
        };
        payload.push(u8::from(msg.typ));
        let nonce = aead::Nonce::<AesGcm<aes_gcm::aes::Aes256, typenum::U12>>::from_slice(&nonce_block);
        let tag = cipher.encrypt_in_place_detached(nonce, &aad, &mut payload)
            .map_err(|_| rustls::Error::EncryptError)?;
        let mut out_data = Vec::with_capacity(total_len);
        out_data.extend_from_slice(&payload);
        out_data.extend_from_slice(tag.as_slice());
        Ok(OutboundOpaqueMessage { typ: ContentType::ApplicationData, version: ProtocolVersion::TLSv1_2, payload: PrefixedPayload::from(out_data.as_slice()) })
    }
    fn encrypted_payload_len(&self, payload_len: usize) -> usize { payload_len + 1 + GCM_TAG_LEN }
}

impl MessageDecrypter for Aes256GcmImpl {
    fn decrypt<'a>(&mut self, mut msg: InboundOpaqueMessage<'a>, seq: u64) -> Result<InboundPlainMessage<'a>, rustls::Error> {
        if msg.payload.len() < 1 + GCM_TAG_LEN {
            return Err(rustls::Error::DecryptError);
        }

        let aad = tls13_aad(msg.payload.len(), seq);
        let nonce_block = tls13_nonce(&self.iv, seq);

        let cipher = AesGcm::<aes_gcm::aes::Aes256, typenum::U12>::new_from_slice(&self.key)
            .map_err(|_| rustls::Error::DecryptError)?;
        let nonce = aead::Nonce::<AesGcm<aes_gcm::aes::Aes256, typenum::U12>>::from_slice(&nonce_block);

        let ct_body_len = msg.payload.len() - 1 - GCM_TAG_LEN;
        let tag_bytes = msg.payload[msg.payload.len() - GCM_TAG_LEN..].to_vec();

        {
            let buf = &mut *msg.payload;
            let (_, ct_and_tag) = buf.split_at_mut(1);
            let (ciphertext, _tag_region) = ct_and_tag.split_at_mut(ct_body_len);
            let tag_slice = aead::Tag::<AesGcm<aes_gcm::aes::Aes256, typenum::U12>>::from_slice(&tag_bytes);
            cipher.decrypt_in_place_detached(nonce, &aad, ciphertext, tag_slice)
                .map_err(|_| rustls::Error::DecryptError)?;
        }

        msg.payload.truncate(1 + ct_body_len);
        msg.into_tls13_unpadded_message()
    }
}

impl fmt::Debug for CactAes128Gcm {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result { write!(f, "CactAes128Gcm") }
}
impl fmt::Debug for CactAes256Gcm {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result { write!(f, "CactAes256Gcm") }
}
