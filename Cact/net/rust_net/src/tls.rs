//! TLS FFI: full handshake and data transfer via rustls UnbufferedClientConnection.

use alloc::sync::Arc;
use alloc::vec::Vec;
use core::ffi::{c_int, c_void};

use cact_crypto::provider::cact_crypto_provider;
use rustls::pki_types::{ServerName, UnixTime};
use rustls::time_provider::TimeProvider;
use rustls::ClientConfig;
use rustls::client::UnbufferedClientConnection;
use rustls::conn::unbuffered::ConnectionState;

const TLS_MAX_CONNECTIONS: usize = 4;

struct TlsConn {
    conn: UnbufferedClientConnection,
    sock: c_int,
    inbuf: Vec<u8>,
    outbuf: Vec<u8>,
    ready_plaintext: Vec<u8>,
    plaintext_off: usize,
}

static mut TLS_CONNS: [Option<TlsConn>; TLS_MAX_CONNECTIONS] = [const { None }; TLS_MAX_CONNECTIONS];

const BUF_SIZE: usize = 16384;

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

fn make_config() -> Arc<ClientConfig> {
    let provider = cact_crypto_provider();
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

// SAFETY: backed by C tcp_send/tcp_recv implementations.
unsafe extern "C" {
    fn tcp_send(sock: i32, data: *mut u8, len: u16) -> i32;
    fn tcp_recv(sock: c_int, buf: *mut u8, max_len: u16) -> i32;
}

fn tcp_read(sock: i32, buf: &mut [u8]) -> Result<usize, ()> {
    let max = buf.len().min(u16::MAX as usize) as u16;
    let n = unsafe { tcp_recv(sock, buf.as_mut_ptr(), max) };
    if n < 0 { Err(()) } else { Ok(n as usize) }
}

fn tcp_write(sock: i32, buf: &[u8]) -> Result<(), ()> {
    let max = buf.len().min(u16::MAX as usize) as u16;
    let n = unsafe { tcp_send(sock, buf.as_ptr() as *mut u8, max) };
    if n < 0 { Err(()) } else { Ok(()) }
}

fn remove_front(v: &mut Vec<u8>, n: usize) {
    if n > 0 && n <= v.len() {
        v.copy_within(n.., 0);
        v.truncate(v.len() - n);
    }
}

/// Drain accumulated plaintext into `dst`.
fn drain_pt(tls: &mut TlsConn, dst: &mut [u8]) -> usize {
    if tls.plaintext_off >= tls.ready_plaintext.len() {
        tls.ready_plaintext.clear();
        tls.plaintext_off = 0;
        return 0;
    }
    let avail = &tls.ready_plaintext[tls.plaintext_off..];
    let n = dst.len().min(avail.len());
    dst[..n].copy_from_slice(&avail[..n]);
    tls.plaintext_off += n;
    if tls.plaintext_off >= tls.ready_plaintext.len() {
        tls.ready_plaintext.clear();
        tls.plaintext_off = 0;
    }
    n
}

/// Append new data to inbuf after existing content. Returns bytes appended.
fn append_to_inbuf(tls: &mut TlsConn) -> Result<usize, ()> {
    let old_len = tls.inbuf.len();
    tls.inbuf.resize(old_len + BUF_SIZE, 0);
    let n = tcp_read(tls.sock, &mut tls.inbuf[old_len..])?;
    tls.inbuf.truncate(old_len + n);
    Ok(n)
}

/// Drive the TLS state machine: handle one record, do IO as needed.
/// Returns Ok(true) if more steps, Ok(false) if done/idle, Err(()) on error.
fn step(conn: &mut UnbufferedClientConnection, inbuf: &mut Vec<u8>, outbuf: &mut Vec<u8>, pt: &mut Vec<u8>, sock: i32) -> Result<bool, ()> {
    outbuf.resize(BUF_SIZE, 0);

    let status = conn.process_tls_records(&mut inbuf[..]);
    let discard = status.discard;

    let keep_going = match status.state {
        Ok(ConnectionState::EncodeTlsData(mut encoder)) => {
            if let Ok(n) = encoder.encode(outbuf) {
                let _ = tcp_write(sock, &outbuf[..n]);
            }
            remove_front(inbuf, discard);
            true
        }
        Ok(ConnectionState::TransmitTlsData(transmit)) => {
            transmit.done();
            remove_front(inbuf, discard);
            true
        }
        Ok(ConnectionState::BlockedHandshake) => {
            remove_front(inbuf, discard);
            let old = inbuf.len();
            inbuf.resize(old + BUF_SIZE, 0);
            let n = tcp_read(sock, &mut inbuf[old..])?;
            inbuf.truncate(old + n);
            true
        }
        Ok(ConnectionState::WriteTraffic(_w)) => {
            remove_front(inbuf, discard);
            false
        }
        Ok(ConnectionState::ReadTraffic(mut r)) => {
            while let Some(rec) = r.next_record() {
                if let Ok(record) = rec {
                    pt.extend_from_slice(record.payload);
                }
            }
            remove_front(inbuf, discard);
            false
        }
        Ok(ConnectionState::ReadEarlyData(_r)) => {
            remove_front(inbuf, discard);
            true
        }
        Ok(ConnectionState::PeerClosed) | Ok(ConnectionState::Closed) => {
            remove_front(inbuf, discard);
            return Err(());
        }
        Ok(_) => {
            remove_front(inbuf, discard);
            return Err(());
        }
        Err(_) => {
            remove_front(inbuf, discard);
            return Err(());
        }
    };
    Ok(keep_going)
}

fn handshake(conn: &mut UnbufferedClientConnection, inbuf: &mut Vec<u8>, outbuf: &mut Vec<u8>, pt: &mut Vec<u8>, sock: i32) -> Result<(), ()> {
    // Initial data to trigger ClientHello encoding
    if inbuf.is_empty() {
        let old = inbuf.len();
        inbuf.resize(old + BUF_SIZE, 0);
        let n = tcp_read(sock, &mut inbuf[old..])?;
        inbuf.truncate(old + n);
    }
    for _ in 0..256 {
        if !step(conn, inbuf, outbuf, pt, sock)? {
            if !conn.is_handshaking() {
                return Ok(());
            }
        }
    }
    Err(())
}

#[no_mangle]
pub extern "C" fn cact_tls_init() {}

#[no_mangle]
pub extern "C" fn cact_tls_connect(sock: c_int, server_name: *const c_void) -> c_int {
    if sock < 0 { return -1; }

    let name_bytes = unsafe {
        let ptr = server_name as *const u8;
        let mut len = 0usize;
        while *ptr.add(len) != 0 { len += 1; }
        core::slice::from_raw_parts(ptr, len)
    };
    let name_str = match core::str::from_utf8(name_bytes) {
        Ok(s) => s,
        Err(_) => return -1,
    };
    let server_name = match ServerName::try_from(name_str) {
        Ok(n) => n,
        Err(_) => return -1,
    };

    let config = make_config();
    let conn = match UnbufferedClientConnection::new(config, server_name) {
        Ok(c) => c,
        Err(_) => return -1,
    };

    let mut tls = TlsConn {
        conn,
        sock,
        inbuf: Vec::with_capacity(BUF_SIZE),
        outbuf: Vec::with_capacity(BUF_SIZE),
        ready_plaintext: Vec::with_capacity(BUF_SIZE),
        plaintext_off: 0,
    };

    if handshake(&mut tls.conn, &mut tls.inbuf, &mut tls.outbuf, &mut tls.ready_plaintext, sock).is_err() {
        return -1;
    }

    for i in 0..TLS_MAX_CONNECTIONS {
        unsafe {
            if TLS_CONNS[i].is_none() {
                TLS_CONNS[i] = Some(tls);
                return i as c_int;
            }
        }
    }
    -1
}

#[no_mangle]
pub extern "C" fn cact_tls_send(conn_idx: c_int, data: *const u8, len: u16) -> c_int {
    if conn_idx < 0 || conn_idx as usize >= TLS_MAX_CONNECTIONS || data.is_null() { return -1; }
    unsafe {
        let Some(ref mut tls) = TLS_CONNS[conn_idx as usize] else { return -1; };
        let plaintext = core::slice::from_raw_parts(data, len as usize);

        tls.outbuf.resize(BUF_SIZE, 0);

        let status = tls.conn.process_tls_records(&mut tls.inbuf[..]);
        let discard = status.discard;

        let result = match status.state {
            Ok(ConnectionState::WriteTraffic(mut writer)) => {
                match writer.encrypt(plaintext, &mut tls.outbuf) {
                    Ok(n) if n > 0 => {
                        let _ = tcp_write(tls.sock, &tls.outbuf[..n]);
                        len as c_int
                    }
                    Ok(_) => len as c_int,
                    Err(_) => -1,
                }
            }
            Ok(ConnectionState::EncodeTlsData(mut encoder)) => {
                let _ = encoder.encode(&mut tls.outbuf);
                -1
            }
            Ok(ConnectionState::TransmitTlsData(transmit)) => {
                transmit.done();
                -1
            }
            Ok(_) => -1,
            Err(_) => -1,
        };
        remove_front(&mut tls.inbuf, discard);
        result
    }
}

#[no_mangle]
pub extern "C" fn cact_tls_recv(conn_idx: c_int, buf: *mut u8, max_len: u16) -> c_int {
    if conn_idx < 0 || conn_idx as usize >= TLS_MAX_CONNECTIONS || buf.is_null() { return -1; }
    unsafe {
        let Some(ref mut tls) = TLS_CONNS[conn_idx as usize] else { return -1; };
        let dst = core::slice::from_raw_parts_mut(buf, max_len as usize);

        let n = drain_pt(tls, dst);
        if n > 0 { return n as c_int; }

        // Append new TLS data to inbuf
        let old = tls.inbuf.len();
        tls.inbuf.resize(old + BUF_SIZE, 0);
        let nread = tcp_read(tls.sock, &mut tls.inbuf[old..]).unwrap_or(0);
        if nread == 0 { return -1; }
        tls.inbuf.truncate(old + nread);

        tls.outbuf.resize(BUF_SIZE, 0);

        let status = tls.conn.process_tls_records(&mut tls.inbuf[..]);
        let discard = status.discard;

        let result = match status.state {
            Ok(ConnectionState::ReadTraffic(mut reader)) => {
                let mut copied: isize = 0;
                while let Some(rec) = reader.next_record() {
                    if let Ok(record) = rec {
                        let payload = record.payload;
                        let n = dst.len().min(payload.len());
                        if n < payload.len() {
                            let rem = &payload[n..];
                            if tls.ready_plaintext.len() + rem.len() > BUF_SIZE * 4 {
                                copied = -1;
                                break;
                            }
                            dst[..n].copy_from_slice(&payload[..n]);
                            tls.ready_plaintext.extend_from_slice(rem);
                            tls.plaintext_off = 0;
                        } else {
                            dst[..n].copy_from_slice(&payload[..n]);
                        }
                        copied = n as isize;
                        break;
                    }
                }
                copied as c_int
            }
            Ok(ConnectionState::BlockedHandshake) => 0,
            Ok(ConnectionState::EncodeTlsData(mut encoder)) => {
                let _ = encoder.encode(&mut tls.outbuf);
                -1
            }
            Ok(_) => -1,
            Err(_) => -1,
        };
        remove_front(&mut tls.inbuf, discard);
        result
    }
}

#[no_mangle]
pub extern "C" fn cact_tls_close(conn: c_int) {
    if conn >= 0 && (conn as usize) < TLS_MAX_CONNECTIONS {
        unsafe { TLS_CONNS[conn as usize] = None; }
    }
}
