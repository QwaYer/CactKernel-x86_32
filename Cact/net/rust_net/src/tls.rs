//! TLS via vendored rustls: full handshake and application data transfer over a
//! connected TCP socket, driven by a poll/sleep loop so it works with the
//! non-blocking smoltcp-backed `tcp_*` layer.
//!
//! Two configs are available:
//!   * verified   — webpki roots, standard chain verification.  NOTE: the
//!     in-kernel `cact_crypto` provider does not implement certificate
//!     signature verification yet, so this path only succeeds against servers
//!     with certificates that skip signature checks.
//!   * skip-verify — `dangerous()` custom verifier that accepts any chain.
//!     Used by the HTTP client by default until real verification lands.
//!
//! The C ABI (`cact_tls_connect/send/recv/close`) is preserved for external
//! callers; the in-crate API (`TlsStream`) is what the HTTP client uses.

use alloc::string::String;
use alloc::sync::Arc;
use alloc::vec;
use alloc::vec::Vec;
use core::ffi::{c_int, c_void};

use cact_crypto::provider::cact_crypto_provider;
use rustls::client::UnbufferedClientConnection;
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::conn::unbuffered::ConnectionState;
use rustls::pki_types::{CertificateDer, ServerName, UnixTime};
use rustls::time_provider::TimeProvider;
use rustls::{ClientConfig, DigitallySignedStruct, Error, SignatureScheme};

use crate::ffi_kernel;

const TLS_MAX_CONNECTIONS: usize = 4;
const BUF_SIZE: usize = 16384;
const HANDSHAKE_TIMEOUT_TICKS: u32 = 600;
const IO_TIMEOUT_TICKS: u32 = 600;

/// Read outcomes for [`TlsStream`] (non-blocking style).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum TlsRead {
    /// `n` plaintext bytes were copied into the caller buffer.
    Data(usize),
    /// No plaintext available right now; try again after a short sleep.
    WouldBlock,
    /// Peer sent close_notify (or the connection is fully closed).
    Closed,
    /// Fatal TLS or transport error.
    Err,
}

/// TLS connection bound to an open, established TCP socket index (`sock`).
pub(crate) struct TlsStream {
    conn: UnbufferedClientConnection,
    sock: c_int,
    inbuf: Vec<u8>,
    outbuf: Vec<u8>,
    ready_plaintext: Vec<u8>,
    plaintext_off: usize,
}

#[derive(Debug)]
struct CactTimeProvider;

impl TimeProvider for CactTimeProvider {
    fn current_time(&self) -> Option<UnixTime> {
        let now = cact_shim::time::SystemTime::now()
            .duration_since(cact_shim::time::UNIX_EPOCH)
            .ok()?;
        Some(UnixTime::since_unix_epoch(core::time::Duration::from_secs(now.as_secs())))
    }
}

/// Accepts every certificate chain.  Only for environments where the in-kernel
/// crypto provider cannot verify certificate signatures yet.
#[derive(Debug)]
struct NoVerifyVerifier;

impl ServerCertVerifier for NoVerifyVerifier {
    fn verify_server_cert(
        &self,
        _end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp_response: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, Error> {
        Ok(ServerCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        _message: &[u8],
        _cert: &CertificateDer<'_>,
        _dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, Error> {
        Ok(HandshakeSignatureValid::assertion())
    }

    fn verify_tls13_signature(
        &self,
        _message: &[u8],
        _cert: &CertificateDer<'_>,
        _dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, Error> {
        Ok(HandshakeSignatureValid::assertion())
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        vec![
            SignatureScheme::RSA_PSS_SHA256,
            SignatureScheme::RSA_PSS_SHA384,
            SignatureScheme::RSA_PSS_SHA512,
            SignatureScheme::RSA_PKCS1_SHA256,
            SignatureScheme::RSA_PKCS1_SHA384,
            SignatureScheme::RSA_PKCS1_SHA512,
            SignatureScheme::ECDSA_NISTP256_SHA256,
            SignatureScheme::ECDSA_NISTP384_SHA384,
        ]
    }
}

fn make_config(skip_verify: bool) -> Arc<ClientConfig> {
    let provider = cact_crypto_provider();
    if skip_verify {
        return Arc::new(
            ClientConfig::builder_with_details(Arc::new(provider), Arc::new(CactTimeProvider))
                .with_safe_default_protocol_versions()
                .unwrap()
                .dangerous()
                .with_custom_certificate_verifier(Arc::new(NoVerifyVerifier))
                .with_no_client_auth(),
        );
    }
    let mut root_store = rustls::RootCertStore::empty();
    for ta in webpki_roots::TLS_SERVER_ROOTS.iter() {
        root_store.roots.push(rustls::pki_types::TrustAnchor {
            subject: ta.subject.as_ref().into(),
            subject_public_key_info: ta.subject_public_key_info.as_ref().into(),
            name_constraints: ta.name_constraints.as_ref().map(|nc| nc.as_ref().into()),
        });
    }
    Arc::new(
        ClientConfig::builder_with_details(Arc::new(provider), Arc::new(CactTimeProvider))
            .with_safe_default_protocol_versions()
            .unwrap()
            .with_root_certificates(root_store)
            .with_no_client_auth(),
    )
}

// SAFETY: backed by the C-ABI tcp_recv exported by this crate.
unsafe extern "C" {
    fn tcp_recv(sock: c_int, buf: *mut u8, max_len: u16) -> i32;
}

/// Current kernel tick counter (safe wrapper).
fn now_ticks() -> u32 {
    // SAFETY: ffi_kernel re-exports the C timer.
    unsafe { ffi_kernel::timer_ticks_get() }
}

/// Sleep for `t` ticks (safe wrapper).
fn sleep_ticks(t: u32) {
    // SAFETY: ffi_kernel re-exports the C scheduler sleep.
    unsafe { ffi_kernel::sched_sleep_ticks(t) }
}

/// True when the underlying TCP socket reached a closed state (FIN received or
/// fully closed) — used to surface EOF even when the peer omits close_notify.
fn tcp_socket_eof(sock: i32) -> bool {
    use smoltcp::socket::tcp as stcp;
    crate::tcp::with_tcp_socket(sock, |s| {
        matches!(
            s.state(),
            stcp::State::CloseWait | stcp::State::Closed | stcp::State::TimeWait
        )
    })
    .unwrap_or(true)
}

/// Write `buf` to the TCP socket in small chunks, sleeping briefly when the
/// smoltcp TX buffer is full. Advances by the actual number of bytes the
/// socket accepted.
fn tcp_write(sock: i32, buf: &[u8]) -> Result<(), ()> {
    let deadline = now_ticks().saturating_add(IO_TIMEOUT_TICKS);
    let mut off = 0usize;
    while off < buf.len() {
        let chunk = core::cmp::min(1400, buf.len() - off);
        let sent = crate::tcp::with_tcp_socket(sock, |s| {
            if !s.may_send() {
                return 0;
            }
            match s.send_slice(&buf[off..off + chunk]) {
                Ok(n) => n,
                Err(_) => 0,
            }
        })
        .unwrap_or(0);
        if sent > 0 {
            off += sent;
            continue;
        }
        if now_ticks() >= deadline {
            return Err(());
        }
        sleep_ticks(1);
    }
    Ok(())
}

/// Read up to `buf.len()` encrypted bytes from the TCP socket (0 means nothing
/// available right now; the caller polls again later).
fn tcp_read(sock: i32, buf: &mut [u8]) -> Result<usize, ()> {
    let max = core::cmp::min(buf.len(), u16::MAX as usize) as u16;
    let n = unsafe { tcp_recv(sock, buf.as_mut_ptr(), max) };
    if n < 0 {
        Err(())
    } else {
        Ok(n as usize)
    }
}

fn remove_front(v: &mut Vec<u8>, n: usize) {
    if n > 0 && n <= v.len() {
        v.copy_within(n.., 0);
        v.truncate(v.len() - n);
    }
}

/// Drain accumulated plaintext into `dst`. Returns bytes copied.
fn drain_pt(tls: &mut TlsStream, dst: &mut [u8]) -> usize {
    if tls.plaintext_off >= tls.ready_plaintext.len() {
        tls.ready_plaintext.clear();
        tls.plaintext_off = 0;
        return 0;
    }
    let avail = &tls.ready_plaintext[tls.plaintext_off..];
    let n = core::cmp::min(dst.len(), avail.len());
    dst[..n].copy_from_slice(&avail[..n]);
    tls.plaintext_off += n;
    if tls.plaintext_off >= tls.ready_plaintext.len() {
        tls.ready_plaintext.clear();
        tls.plaintext_off = 0;
    }
    n
}

/// Append newly received encrypted bytes to `inbuf`. Returns bytes appended
/// (0 is fine; the caller polls again later).
pub(crate) fn append_to_inbuf(tls: &mut TlsStream) -> Result<usize, ()> {
    let mut scratch = [0u8; 4096];
    let n = tcp_read(tls.sock, &mut scratch)?;
    tls.inbuf.extend_from_slice(&scratch[..n]);
    Ok(n)
}

/// Drive the rustls state machine until the handshake completes.
fn handshake(tls: &mut TlsStream) -> Result<(), ()> {
    let deadline = now_ticks().saturating_add(HANDSHAKE_TIMEOUT_TICKS);
    // Prime the state machine with a read attempt so ClientHello gets encoded.
    if tls.inbuf.is_empty() {
        let _ = append_to_inbuf(tls);
    }
    loop {
        if now_ticks() >= deadline {
            return Err(());
        }
        tls.outbuf.resize(BUF_SIZE, 0);
        let status = tls.conn.process_tls_records(&mut tls.inbuf[..]);
        let discard = status.discard;
        match status.state {
            Ok(ConnectionState::EncodeTlsData(mut encoder)) => {
                if let Ok(n) = encoder.encode(&mut tls.outbuf) {
                    if n > 0 {
                        tcp_write(tls.sock, &tls.outbuf[..n])?;
                    }
                }
                remove_front(&mut tls.inbuf, discard);
            }
            Ok(ConnectionState::TransmitTlsData(transmit)) => {
                transmit.done();
                remove_front(&mut tls.inbuf, discard);
            }
            Ok(ConnectionState::BlockedHandshake) => {
                remove_front(&mut tls.inbuf, discard);
                let _ = append_to_inbuf(tls);
                sleep_ticks(1);
            }
            Ok(ConnectionState::WriteTraffic(_))
            | Ok(ConnectionState::ReadTraffic(_))
            | Ok(ConnectionState::ReadEarlyData(_)) => {
                remove_front(&mut tls.inbuf, discard);
                if !tls.conn.is_handshaking() {
                    return Ok(());
                }
            }
            Ok(ConnectionState::PeerClosed) | Ok(ConnectionState::Closed) => {
                remove_front(&mut tls.inbuf, discard);
                return Err(());
            }
            Ok(_) => {
                remove_front(&mut tls.inbuf, discard);
                return Err(());
            }
            Err(_) => {
                remove_front(&mut tls.inbuf, discard);
                return Err(());
            }
        }
    }
}

/// Open a TLS stream over an already-connected TCP socket `sock` (socket index).
/// `server_name` is the SNI/hostname. `skip_verify` disables certificate checks.
pub(crate) fn tls_stream_open(sock: c_int, server_name: &str, skip_verify: bool) -> Option<TlsStream> {
    if sock < 0 {
        return None;
    }
    let server_name = ServerName::try_from(String::from(server_name)).ok()?;
    let config = make_config(skip_verify);
    let conn = UnbufferedClientConnection::new(config, server_name).ok()?;
    let mut tls = TlsStream {
        conn,
        sock,
        inbuf: Vec::with_capacity(BUF_SIZE),
        outbuf: Vec::with_capacity(BUF_SIZE),
        ready_plaintext: Vec::with_capacity(BUF_SIZE),
        plaintext_off: 0,
    };
    handshake(&mut tls).ok()?;
    Some(tls)
}

/// Send plaintext application data (fragmented into TLS records).
pub(crate) fn tls_stream_write(tls: &mut TlsStream, data: &[u8]) -> Result<(), ()> {
    let deadline = now_ticks().saturating_add(IO_TIMEOUT_TICKS);
    let mut off = 0usize;
    while off < data.len() {
        let chunk = core::cmp::min(1536, data.len() - off);
        tls.outbuf.resize(BUF_SIZE, 0);
        let status = tls.conn.process_tls_records(&mut tls.inbuf[..]);
        let discard = status.discard;
        match status.state {
            Ok(ConnectionState::WriteTraffic(mut writer)) => {
                match writer.encrypt(&data[off..off + chunk], &mut tls.outbuf) {
                    Ok(n) => {
                        remove_front(&mut tls.inbuf, discard);
                        if n > 0 {
                            tcp_write(tls.sock, &tls.outbuf[..n])?;
                        }
                        off += chunk;
                    }
                    Err(_) => {
                        remove_front(&mut tls.inbuf, discard);
                        return Err(());
                    }
                }
            }
            Ok(ConnectionState::EncodeTlsData(mut encoder)) => {
                let _ = encoder.encode(&mut tls.outbuf);
                remove_front(&mut tls.inbuf, discard);
                return Err(());
            }
            Ok(ConnectionState::TransmitTlsData(transmit)) => {
                transmit.done();
                remove_front(&mut tls.inbuf, discard);
            }
            Ok(ConnectionState::BlockedHandshake) => {
                remove_front(&mut tls.inbuf, discard);
                if now_ticks() >= deadline {
                    return Err(());
                }
                sleep_ticks(1);
            }
            Ok(ConnectionState::PeerClosed) | Ok(ConnectionState::Closed) => {
                remove_front(&mut tls.inbuf, discard);
                return Err(());
            }
            Ok(_) => {
                remove_front(&mut tls.inbuf, discard);
                return Err(());
            }
            Err(_) => {
                remove_front(&mut tls.inbuf, discard);
                return Err(());
            }
        }
    }
    Ok(())
}

/// Read plaintext application data. Caller should try `append_to_inbuf` first
/// to give the state machine fresh encrypted input.
pub(crate) fn tls_stream_read(tls: &mut TlsStream, dst: &mut [u8]) -> TlsRead {
    let n = drain_pt(tls, dst);
    if n > 0 {
        return TlsRead::Data(n);
    }
    tls.outbuf.resize(BUF_SIZE, 0);
    let status = tls.conn.process_tls_records(&mut tls.inbuf[..]);
    let discard = status.discard;
    let result = match status.state {
        Ok(ConnectionState::ReadTraffic(mut reader)) => {
            let mut copied = 0usize;
            while let Some(rec) = reader.next_record() {
                if let Ok(record) = rec {
                    let payload = record.payload;
                    if copied + payload.len() <= dst.len() {
                        dst[copied..copied + payload.len()].copy_from_slice(payload);
                        copied += payload.len();
                    } else {
                        let n = dst.len() - copied;
                        dst[copied..].copy_from_slice(&payload[..n]);
                        tls.ready_plaintext.extend_from_slice(&payload[n..]);
                        tls.plaintext_off = 0;
                        copied = dst.len();
                        break;
                    }
                }
            }
            remove_front(&mut tls.inbuf, discard);
            if copied > 0 {
                TlsRead::Data(copied)
            } else {
                TlsRead::WouldBlock
            }
        }
        Ok(ConnectionState::PeerClosed) | Ok(ConnectionState::Closed) => {
            remove_front(&mut tls.inbuf, discard);
            let n = drain_pt(tls, dst);
            if n > 0 {
                TlsRead::Data(n)
            } else {
                TlsRead::Closed
            }
        }
        Ok(ConnectionState::BlockedHandshake) => {
            remove_front(&mut tls.inbuf, discard);
            TlsRead::WouldBlock
        }
        Ok(ConnectionState::WriteTraffic(_)) => {
            // Idle after handshake with no pending plaintext and no new
            // encrypted data — not an error.
            remove_front(&mut tls.inbuf, discard);
            TlsRead::WouldBlock
        }
        Ok(ConnectionState::EncodeTlsData(mut encoder)) => {
            let _ = encoder.encode(&mut tls.outbuf);
            remove_front(&mut tls.inbuf, discard);
            TlsRead::WouldBlock
        }
        Ok(ConnectionState::TransmitTlsData(transmit)) => {
            transmit.done();
            remove_front(&mut tls.inbuf, discard);
            TlsRead::WouldBlock
        }
        Ok(_) => {
            remove_front(&mut tls.inbuf, discard);
            TlsRead::Err
        }
        Err(_) => {
            remove_front(&mut tls.inbuf, discard);
            TlsRead::Err
        }
    };
    if result == TlsRead::WouldBlock && tcp_socket_eof(tls.sock) {
        // A peer that closes TCP without close_notify (common with
        // `Connection: close`) never transitions rustls to PeerClosed/Closed;
        // surface EOF ourselves once the underlying socket is gone.
        TlsRead::Closed
    } else {
        result
    }
}

// ──────────────────────────────── C ABI ────────────────────────────────────

static mut TLS_CONNS: [Option<TlsStream>; TLS_MAX_CONNECTIONS] = [const { None }; TLS_MAX_CONNECTIONS];

/// Read a NUL-terminated C string (bounded).
unsafe fn cstr(ptr: *const c_void) -> Option<alloc::string::String> {
    let p = ptr as *const u8;
    let mut len = 0usize;
    while *p.add(len) != 0 {
        len += 1;
        if len > 1024 {
            return None;
        }
    }
    let slice = core::slice::from_raw_parts(p, len);
    core::str::from_utf8(slice).ok().map(|s| String::from(s))
}

#[no_mangle]
pub extern "C" fn cact_tls_init() {}

/// Connect TLS over an open connected TCP socket. Legacy entry: uses the
/// verified (root store) path.
#[no_mangle]
pub extern "C" fn cact_tls_connect(sock: c_int, server_name: *const c_void) -> c_int {
    cact_tls_connect_ex(sock, server_name, 0)
}

/// Connect TLS over an open connected TCP socket. `skip_verify` != 0 disables
/// certificate chain verification (see module docs). Returns a connection
/// handle >= 0 to use with [`cact_tls_send`]/[`cact_tls_recv`], or -1.
#[no_mangle]
pub extern "C" fn cact_tls_connect_ex(
    sock: c_int,
    server_name: *const c_void,
    skip_verify: c_int,
) -> c_int {
    if sock < 0 || server_name.is_null() {
        return -1;
    }
    // SAFETY: server_name is a NUL-terminated C string.
    let Some(name) = (unsafe { cstr(server_name) }) else {
        return -1;
    };
    let Some(stream) = tls_stream_open(sock, &name, skip_verify != 0) else {
        return -1;
    };
    // SAFETY: single-threaded boot paths; slots are only taken while running.
    unsafe {
        for i in 0..TLS_MAX_CONNECTIONS {
            if TLS_CONNS[i].is_none() {
                TLS_CONNS[i] = Some(stream);
                return i as c_int;
            }
        }
    }
    -1
}

/// Send plaintext data over a TLS connection. Returns bytes accepted, or -1.
#[no_mangle]
pub extern "C" fn cact_tls_send(conn_idx: c_int, data: *const u8, len: u16) -> c_int {
    if conn_idx < 0 || conn_idx as usize >= TLS_MAX_CONNECTIONS || data.is_null() {
        return -1;
    }
    // SAFETY: indices and pointer validated above.
    unsafe {
        let Some(ref mut stream) = TLS_CONNS[conn_idx as usize] else {
            return -1;
        };
        let plaintext = core::slice::from_raw_parts(data, len as usize);
        if tls_stream_write(stream, plaintext).is_ok() {
            len as c_int
        } else {
            -1
        }
    }
}

/// Receive plaintext data from a TLS connection. Returns bytes read, 0 when no
/// data is available yet, or -1 on error/close.
#[no_mangle]
pub extern "C" fn cact_tls_recv(conn_idx: c_int, buf: *mut u8, max_len: u16) -> c_int {
    if conn_idx < 0 || conn_idx as usize >= TLS_MAX_CONNECTIONS || buf.is_null() {
        return -1;
    }
    // SAFETY: indices and pointer validated above.
    unsafe {
        let Some(ref mut stream) = TLS_CONNS[conn_idx as usize] else {
            return -1;
        };
        let dst = core::slice::from_raw_parts_mut(buf, max_len as usize);
        if append_to_inbuf(stream).is_err() {
            return -1;
        }
        match tls_stream_read(stream, dst) {
            TlsRead::Data(n) => n as c_int,
            TlsRead::WouldBlock => 0,
            TlsRead::Closed => {
                if stream.ready_plaintext.len() > stream.plaintext_off {
                    drain_pt(stream, dst) as c_int
                } else {
                    -1
                }
            }
            TlsRead::Err => -1,
        }
    }
}

/// Close and free a TLS connection (does not close the underlying TCP socket).
#[no_mangle]
pub extern "C" fn cact_tls_close(conn: c_int) {
    if conn >= 0 && (conn as usize) < TLS_MAX_CONNECTIONS {
        // SAFETY: validated index.
        unsafe { TLS_CONNS[conn as usize] = None; }
    }
}
