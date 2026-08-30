//! HTTP/1.1 client for in-kernel use: `http://` and `https://` requests over
//! smoltcp TCP sockets, with TLS handled by rustls (see [`crate::tls`]).
//!
//! The entry points are C ABI so the rest of the kernel (syscalls, drivers)
//! can fetch pages over plain HTTP or HTTPS:
//!
//! * [`cact_http_request`]  — generic method (GET/POST/PUT/DELETE/HEAD)
//! * [`cact_http_get`]      — GET convenience
//! * [`cact_http_post`]     — POST convenience
//!
//! HTTPS uses the in-kernel rustls provider.  Certificate chain verification
//! is currently disabled (`skip_verify`) because `cact_crypto` does not
//! implement certificate signature verification yet; this is logged once at
//! startup and should be flipped to a verified config when signatures land.
//!
//! Response handling supports `Content-Length`, chunked `Transfer-Encoding`
//! (including trailers), and "read until close" bodies.

use alloc::vec::Vec;
use core::ffi::{c_char, c_int, c_void};
use core::slice;
use core::sync::atomic::{AtomicBool, Ordering};

use smoltcp::socket::tcp as stcp;

use crate::dns_resolve;
use crate::ffi_kernel;
use crate::stack;
use crate::tcp;
use crate::tls::{self, TlsRead, TlsStream};

/// HTTP methods (match `cact_http_method_t` in `rust_net_ffi.h`).
pub const HTTP_GET: c_int = 1;
pub const HTTP_POST: c_int = 2;
pub const HTTP_PUT: c_int = 3;
pub const HTTP_DELETE: c_int = 4;
pub const HTTP_HEAD: c_int = 5;

/// `flags` for [`cact_http_request`] (match `cact_http_request_flags` in `rust_net_ffi.h`).
/// Verify the TLS certificate chain instead of skipping verification.
pub const CACT_HTTP_FLAG_VERIFY_TLS: u32 = 0x1;

/// Overall per-request deadline in 10 ms ticks (6 s).
const HTTP_TIMEOUT_TICKS: u32 = 600;
/// Safety cap for buffered headers.
const MAX_HEADER_BYTES: usize = 64 * 1024;
/// Safety cap for the whole buffered raw response (headers + body).
const MAX_RESPONSE_BYTES: usize = 1024 * 1024;

/// Response metadata, layout-compatible with `cact_http_resp_t` in `rust_net_ffi.h`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct HttpResp {
    /// HTTP status code (200, 404, ...); 0 if the transport failed.
    pub status: u16,
    /// Byte offset of the body inside `out_buf` (right after the header block).
    pub body_off: u32,
    /// Bytes of the decoded body written into `out_buf`.
    pub body_len: u32,
    /// Total bytes written into `out_buf` (headers + decoded body).
    pub total: u32,
    /// 1 when `out_buf` was too small and output was cut short.
    pub truncated: u8,
    /// 1 when the body was `Transfer-Encoding: chunked` (already decoded).
    pub chunked: u8,
    /// 1 when the exchange used TLS.
    pub tls: u8,
    /// 1 when the request hit the deadline.
    pub timed_out: u8,
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

// ─────────────────────────────── URL parsing ───────────────────────────────

struct HttpUrl {
    tls: bool,
    host: Vec<u8>,
    port: u16,
    path: Vec<u8>,
}

fn trim_ws(mut b: &[u8]) -> &[u8] {
    while let Some((&f, rest)) = b.split_first() {
        if f == b' ' || f == b'\t' || f == b'\r' || f == b'\n' {
            b = rest;
        } else {
            break;
        }
    }
    while let Some((&l, rest)) = b.split_last() {
        if l == b' ' || l == b'\t' || l == b'\r' || l == b'\n' {
            b = rest;
        } else {
            break;
        }
    }
    b
}

fn parse_port(port: &[u8]) -> Option<u16> {
    if port.is_empty() || port.len() > 5 {
        return None;
    }
    let mut v: u32 = 0;
    for &b in port {
        if !b.is_ascii_digit() {
            return None;
        }
        v = v * 10 + (b - b'0') as u32;
        if v > 65535 {
            return None;
        }
    }
    Some(v as u16)
}

/// Parse `scheme://host[:port]/path[?query][#frag]` (also tolerates a bare
/// `host/path` which defaults to http). Rejects CR/LF and control bytes so the
/// host/path can be safely embedded in the request line and Host header.
fn parse_url(input: &[u8]) -> Option<HttpUrl> {
    let input = trim_ws(input);
    if input.is_empty() {
        return None;
    }
    let (tls, rest) = if input.starts_with(b"https://") {
        (true, &input[8..])
    } else if input.starts_with(b"http://") {
        (false, &input[7..])
    } else {
        (false, input)
    };
    let (authority, path) = match rest.iter().position(|&b| b == b'/' || b == b'?') {
        Some(i) => {
            let (a, p) = rest.split_at(i);
            (a, p.to_vec())
        }
        None => (rest, b"/".to_vec()),
    };
    let authority = match authority.iter().position(|&b| b == b'@') {
        Some(i) => &authority[i + 1..],
        None => authority,
    };
    let (host, port) = match authority.iter().rposition(|&b| b == b':') {
        Some(i) => (&authority[..i], parse_port(&authority[i + 1..])?),
        None => (authority, if tls { 443 } else { 80 }),
    };
    if host.is_empty() || host.len() > 253 {
        return None;
    }
    if host.iter().any(|&b| b < 0x20 || b == 0x7f) {
        return None;
    }
    let path = if path.first() == Some(&b'?') {
        let mut p = b"/".to_vec();
        p.extend_from_slice(&path);
        p
    } else {
        path
    };
    if path.is_empty() || path.len() > 8192 {
        return None;
    }
    if path.iter().any(|&b| b < 0x20 || b == 0x7f) {
        return None;
    }
    let path = match path.iter().position(|&b| b == b'#') {
        Some(i) => path[..i].to_vec(),
        None => path.to_vec(),
    };
    let path = if path.is_empty() { b"/".to_vec() } else { path };
    Some(HttpUrl {
        tls,
        host: host.to_vec(),
        port,
        path,
    })
}

// ─────────────────────────────── TCP transport ─────────────────────────────

fn tcp_state(idx: i32) -> Option<stcp::State> {
    tcp::with_tcp_socket(idx, |s| s.state())
}

fn wait_established(idx: i32, deadline: u32) -> bool {
    loop {
        if now_ticks() >= deadline {
            return false;
        }
        match tcp_state(idx) {
            Some(stcp::State::Established) => return true,
            Some(stcp::State::Closed) | Some(stcp::State::TimeWait) | None => return false,
            _ => {}
        }
        sleep_ticks(1);
    }
}

fn sock_write(idx: i32, data: &[u8]) -> bool {
    let deadline = now_ticks().saturating_add(HTTP_TIMEOUT_TICKS);
    let mut off = 0usize;
    while off < data.len() {
        let chunk = core::cmp::min(1400, data.len() - off);
        // send_slice returns the number of bytes actually enqueued (can be
        // less than `chunk` when the TX buffer is nearly full).
        let sent = tcp::with_tcp_socket(idx, |s| {
            if !s.may_send() {
                return 0;
            }
            match s.send_slice(&data[off..off + chunk]) {
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
            return false;
        }
        sleep_ticks(1);
    }
    true
}

enum Rd {
    Data(usize),
    Eof,
    Again,
}

fn sock_read(idx: i32, dst: &mut [u8]) -> Rd {
    tcp::with_tcp_socket(idx, |s| match s.state() {
        stcp::State::CloseWait | stcp::State::Closed | stcp::State::TimeWait => {
            match s.recv_slice(dst) {
                Ok(n) if n > 0 => Rd::Data(n),
                _ => Rd::Eof,
            }
        }
        // recv_slice returns Ok(0) when the RX buffer momentarily drains on an
        // open connection; that is NOT end-of-stream, so retry instead.
        _ => match s.recv_slice(dst) {
            Ok(n) if n > 0 => Rd::Data(n),
            _ => Rd::Again,
        },
    })
    .unwrap_or(Rd::Again)
}

// ─────────────────────────────── Pull source ───────────────────────────────

/// Where response bytes come from: a plain TCP socket or a TLS stream.
enum PullSrc<'a> {
    Plain(c_int),
    Tls(&'a mut TlsStream),
}

enum Pull {
    Data(usize),
    Eof,
    Error,
}

/// Blocking pull: sleeps between attempts until data arrives, EOF, or the
/// deadline passes.
fn pull(src: &mut PullSrc, dst: &mut [u8], deadline: u32) -> Pull {
    loop {
        if now_ticks() >= deadline {
            return Pull::Error;
        }
        match src {
            PullSrc::Plain(idx) => match sock_read(*idx, dst) {
                Rd::Data(n) => return Pull::Data(n),
                Rd::Eof => return Pull::Eof,
                Rd::Again => sleep_ticks(1),
            },
            PullSrc::Tls(tls) => {
                if tls::append_to_inbuf(tls).is_err() {
                    return Pull::Error;
                }
                match tls::tls_stream_read(tls, dst) {
                    TlsRead::Data(n) => return Pull::Data(n),
                    TlsRead::Closed => return Pull::Eof,
                    TlsRead::WouldBlock => sleep_ticks(1),
                    TlsRead::Err => return Pull::Error,
                }
            }
        }
    }
}

// ─────────────────────────────── Request build ─────────────────────────────

fn append_decimal(v: u64, out: &mut Vec<u8>) {
    let mut tmp = [0u8; 20];
    let mut n = 0usize;
    let mut x = v;
    if x == 0 {
        out.push(b'0');
        return;
    }
    while x > 0 {
        tmp[n] = b'0' + (x % 10) as u8;
        n += 1;
        x /= 10;
    }
    for i in (0..n).rev() {
        out.push(tmp[i]);
    }
}

fn method_bytes(method: c_int) -> Option<&'static [u8]> {
    match method {
        HTTP_GET => Some(b"GET"),
        HTTP_POST => Some(b"POST"),
        HTTP_PUT => Some(b"PUT"),
        HTTP_DELETE => Some(b"DELETE"),
        HTTP_HEAD => Some(b"HEAD"),
        _ => None,
    }
}

fn build_request(
    method: c_int,
    host: &[u8],
    path: &[u8],
    extra: Option<&[u8]>,
    body: Option<&[u8]>,
) -> Option<Vec<u8>> {
    let m = method_bytes(method)?;
    let mut req = Vec::with_capacity(512);
    req.extend_from_slice(m);
    req.extend_from_slice(b" ");
    req.extend_from_slice(path);
    req.extend_from_slice(b" HTTP/1.1\r\n");
    req.extend_from_slice(b"Host: ");
    req.extend_from_slice(host);
    req.extend_from_slice(b"\r\n");
    req.extend_from_slice(b"User-Agent: CactKernel/2.0 (i686)\r\n");
    req.extend_from_slice(b"Accept: */*\r\n");
    req.extend_from_slice(b"Connection: close\r\n");
    if body.is_some() {
        req.extend_from_slice(b"Content-Length: ");
        append_decimal(body.map_or(0, |b| b.len() as u64), &mut req);
        req.extend_from_slice(b"\r\n");
    }
    if let Some(e) = extra {
        let mut e = e;
        while let Some((&l, rest)) = e.split_last() {
            if l == b'\r' || l == b'\n' {
                e = rest;
            } else {
                break;
            }
        }
        if !e.is_empty() {
            req.extend_from_slice(e);
            req.extend_from_slice(b"\r\n");
        }
    }
    req.extend_from_slice(b"\r\n");
    if let Some(b) = body {
        req.extend_from_slice(b);
    }
    Some(req)
}

// ─────────────────────────────── Response parse ────────────────────────────

fn find_header_end(buf: &[u8]) -> Option<usize> {
    buf.windows(4).position(|w| w == b"\r\n\r\n")
}

fn eq_ignore_ascii_case(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len() && a.iter().zip(b).all(|(x, y)| x.eq_ignore_ascii_case(y))
}

fn contains_ignore_ascii_case(hay: &[u8], needle: &[u8]) -> bool {
    if needle.is_empty() {
        return true;
    }
    hay.windows(needle.len())
        .any(|w| w.iter().zip(needle).all(|(x, y)| x.eq_ignore_ascii_case(y)))
}

fn parse_decimal(b: &[u8]) -> Option<u64> {
    if b.is_empty() {
        return None;
    }
    let mut v: u64 = 0;
    for &c in b {
        if !c.is_ascii_digit() {
            return None;
        }
        v = v.checked_mul(10)?.checked_add((c - b'0') as u64)?;
    }
    Some(v)
}

fn parse_status_line(header: &[u8]) -> u16 {
    let first_line = header.split(|&b| b == b'\n').next().unwrap_or(header);
    let first_line = first_line.strip_suffix(b"\r").unwrap_or(first_line);
    match first_line.iter().position(|&b| b == b' ') {
        Some(sp) => {
            let rest = &first_line[sp + 1..];
            let mut code: u16 = 0;
            for &b in rest.iter().take(3) {
                if !b.is_ascii_digit() {
                    return 0;
                }
                code = code * 10 + (b - b'0') as u16;
            }
            code
        }
        None => 0,
    }
}

fn parse_headers(header: &[u8]) -> (Option<u64>, bool) {
    let mut content_len: Option<u64> = None;
    let mut chunked = false;
    for line in header.split(|&b| b == b'\n') {
        let line = line.strip_suffix(b"\r").unwrap_or(line);
        if line.is_empty() {
            continue;
        }
        let Some(colon) = line.iter().position(|&b| b == b':') else {
            continue;
        };
        let (name, value) = (&line[..colon], trim_ws(&line[colon + 1..]));
        if eq_ignore_ascii_case(name, b"content-length") {
            if let Some(v) = parse_decimal(value) {
                content_len = Some(v);
            }
        } else if eq_ignore_ascii_case(name, b"transfer-encoding") {
            if contains_ignore_ascii_case(value, b"chunked") {
                chunked = true;
            }
        }
    }
    // RFC 7230 §3.3.3: Transfer-Encoding overrides Content-Length.
    if chunked {
        (None, true)
    } else {
        (content_len, false)
    }
}

fn find_lf(buf: &[u8], from: usize) -> Option<usize> {
    buf[from..].iter().position(|&b| b == b'\n').map(|i| from + i)
}

fn parse_hex(b: &[u8]) -> Option<usize> {
    if b.is_empty() || b.len() > 8 {
        return None;
    }
    let mut v: usize = 0;
    for &c in b {
        let d = match c {
            b'0'..=b'9' => (c - b'0') as usize,
            b'a'..=b'f' => (c - b'a' + 10) as usize,
            b'A'..=b'F' => (c - b'A' + 10) as usize,
            _ => return None,
        };
        v = v.checked_mul(16)?.checked_add(d)?;
    }
    Some(v)
}

/// Largest chunk size we accept; keeps all chunk arithmetic safe on 32-bit
/// and bounded by the response cap.
const MAX_CHUNK_BYTES: usize = MAX_RESPONSE_BYTES;

enum ChunkStatus {
    Complete,
    Incomplete,
    Error,
}

/// Incremental chunked-body scanner. `scanner.off` is the next byte offset to
/// parse, so chunks already validated are never re-walked (O(N) overall).
#[derive(Default)]
struct ChunkScanner {
    off: usize,
    trailers: bool,
}

/// Scan a chunked body, resuming from `scanner.off`. Returns `Complete` only
/// when the terminal `0\r\n` chunk and any trailing headers are fully present.
fn chunked_scan(body: &[u8], scanner: &mut ChunkScanner) -> ChunkStatus {
    loop {
        if scanner.trailers {
            let Some(te) = find_lf(body, scanner.off) else {
                return ChunkStatus::Incomplete;
            };
            if te == scanner.off || (te == scanner.off + 1 && body.get(scanner.off) == Some(&b'\r'))
            {
                return ChunkStatus::Complete;
            }
            scanner.off = te + 1;
            continue;
        }
        let line_start = scanner.off;
        let Some(line_end) = find_lf(body, line_start) else {
            return ChunkStatus::Incomplete;
        };
        let line = &body[line_start..line_end];
        let line = line.strip_suffix(b"\r").unwrap_or(line);
        let size_str = match line.iter().position(|&b| b == b';') {
            Some(p) => &line[..p],
            None => line,
        };
        let Some(size) = parse_hex(size_str) else {
            return ChunkStatus::Error;
        };
        if size > MAX_CHUNK_BYTES {
            return ChunkStatus::Error;
        }
        let data_start = line_end + 1;
        if size == 0 {
            scanner.off = data_start;
            scanner.trailers = true;
            continue;
        }
        let Some(chunk_end) = data_start.checked_add(size) else {
            return ChunkStatus::Error;
        };
        let Some(data_end) = chunk_end.checked_add(2) else {
            return ChunkStatus::Error;
        };
        if body.len() < data_end {
            // Cursor stays at the size line so the same line is re-parsed once
            // the chunk data has arrived.
            return ChunkStatus::Incomplete;
        }
        if &body[chunk_end..chunk_end + 2] != b"\r\n" {
            return ChunkStatus::Error;
        }
        scanner.off = data_end;
    }
}

/// Decode a complete chunked body (as produced by `read_raw_response`) into
/// `out`. Returns (bytes written, truncated).
fn decode_chunked(body: &[u8], out: &mut [u8]) -> (usize, bool) {
    let mut i = 0usize;
    let mut w = 0usize;
    loop {
        let Some(line_end) = find_lf(body, i) else {
            break;
        };
        let line = &body[i..line_end];
        let line = line.strip_suffix(b"\r").unwrap_or(line);
        let size_str = match line.iter().position(|&b| b == b';') {
            Some(p) => &line[..p],
            None => line,
        };
        let Some(size) = parse_hex(size_str) else {
            break;
        };
        i = line_end + 1;
        if size == 0 {
            break;
        }
        if size > MAX_CHUNK_BYTES {
            break;
        }
        let Some(chunk_end) = i.checked_add(size) else {
            break;
        };
        let Some(data_end) = chunk_end.checked_add(2) else {
            break;
        };
        if body.len() < data_end {
            break;
        }
        if &body[chunk_end..chunk_end + 2] != b"\r\n" {
            break;
        }
        let Some(w_end) = w.checked_add(size) else {
            break;
        };
        if w_end > out.len() {
            let n = core::cmp::min(out.len() - w, body.len() - chunk_end);
            out[w..w + n].copy_from_slice(&body[chunk_end..chunk_end + n]);
            return (out.len(), true);
        }
        out[w..w_end].copy_from_slice(&body[chunk_end..chunk_end + size]);
        w = w_end;
        i = data_end;
    }
    (w, false)
}

// ─────────────────────────────── Response read ─────────────────────────────

struct RawResp {
    raw: Vec<u8>,
    hdr_end: usize,
    status: u16,
    chunked: bool,
    content_len: Option<u64>,
}

/// Read the full raw response (headers + body as transmitted) into a buffer,
/// stopping when the entity is complete. Bounds are enforced by
/// `MAX_RESPONSE_BYTES` / `MAX_HEADER_BYTES`.
fn read_raw_response(src: &mut PullSrc, deadline: u32, resp: &mut HttpResp) -> Result<RawResp, c_int> {
    let mut raw: Vec<u8> = Vec::with_capacity(4096);
    let mut hdr_end: Option<usize> = None;
    let mut status: u16 = 0;
    let mut chunked = false;
    let mut content_len: Option<u64> = None;
    let mut chunk_scanner = ChunkScanner::default();

    loop {
        if now_ticks() >= deadline {
            resp.timed_out = 1;
            return Err(-1);
        }
        if raw.len() >= MAX_RESPONSE_BYTES {
            resp.truncated = 1;
            break;
        }
        let room = MAX_RESPONSE_BYTES - raw.len();
        let want = core::cmp::min(room, 4096);
        let old = raw.len();
        raw.resize(old + want, 0);
        let n = match pull(src, &mut raw[old..], deadline) {
            Pull::Data(n) => n,
            Pull::Eof => 0,
            Pull::Error => {
                raw.truncate(old);
                return Err(-1);
            }
        };
        raw.truncate(old + n);

        if hdr_end.is_none() {
            // Only scan the newly received window (plus a 3-byte overlap so a
            // terminator split across pulls is still found).
            let scan_from = raw.len().saturating_sub(n + 3).min(old);
            if let Some(rel) = find_header_end(&raw[scan_from..]) {
                let i = scan_from + rel;
                let he = i + 4;
                status = parse_status_line(&raw[..i]);
                let (cl, ch) = parse_headers(&raw[..i]);
                content_len = cl;
                chunked = ch;
                if status == 0 {
                    return Err(-1);
                }
                if he > MAX_HEADER_BYTES {
                    return Err(-1);
                }
                hdr_end = Some(he);
            }
        }

        if let Some(he) = hdr_end {
            if let Some(cl) = content_len {
                // Clamp so a bogus huge Content-Length cannot overflow 32-bit
                // arithmetic; the MAX_RESPONSE_BYTES cap handles the rest.
                let cl_eff = core::cmp::min(cl, MAX_RESPONSE_BYTES as u64) as usize;
                if raw.len() >= he.saturating_add(cl_eff) {
                    break;
                }
            } else if chunked {
                match chunked_scan(&raw[he..], &mut chunk_scanner) {
                    ChunkStatus::Complete => break,
                    ChunkStatus::Error => return Err(-1),
                    ChunkStatus::Incomplete => {}
                }
            }
            // no content-length / not chunked: read until the peer closes
        }

        if n == 0 {
            // EOF: no more bytes are coming. If headers were never parsed this
            // fails below; otherwise the peer closed the response.
            break;
        }
    }

    let he = hdr_end.ok_or(-1)?;
    Ok(RawResp {
        raw,
        hdr_end: he,
        status,
        chunked,
        content_len,
    })
}

fn assemble_response(rr: &RawResp, out_buf: &mut [u8], resp: &mut HttpResp) -> c_int {
    let he = rr.hdr_end;
    if he > out_buf.len() {
        return -2;
    }
    out_buf[..he].copy_from_slice(&rr.raw[..he]);
    let body_avail = &rr.raw[he..];
    let (written, truncated) = if rr.chunked {
        decode_chunked(body_avail, &mut out_buf[he..])
    } else {
        let limit = match rr.content_len {
            Some(cl) => core::cmp::min(cl as usize, body_avail.len()),
            None => body_avail.len(),
        };
        let n = core::cmp::min(limit, out_buf.len() - he);
        out_buf[he..he + n].copy_from_slice(&body_avail[..n]);
        (n, limit > n)
    };
    resp.body_off = he as u32;
    resp.body_len = written as u32;
    resp.total = (he + written) as u32;
    if truncated {
        resp.truncated = 1;
    }
    0
}

fn do_request(
    src: &mut PullSrc,
    deadline: u32,
    out_buf: *mut c_void,
    out_len: u32,
    resp: &mut HttpResp,
) -> c_int {
    let out = unsafe { slice::from_raw_parts_mut(out_buf.cast::<u8>(), out_len as usize) };
    let rr = match read_raw_response(src, deadline, resp) {
        Ok(rr) => rr,
        Err(code) => return code,
    };
    resp.status = rr.status;
    resp.chunked = rr.chunked as u8;
    assemble_response(&rr, out, resp)
}

// ─────────────────────────────── C ABI entry ───────────────────────────────

/// Copy a bounded NUL-terminated C string into a Vec.
unsafe fn cstr_bytes(ptr: *const c_char) -> Option<Vec<u8>> {
    if ptr.is_null() {
        return None;
    }
    let p = ptr.cast::<u8>();
    let mut len = 0usize;
    while *p.add(len) != 0 {
        len += 1;
        if len > 4096 {
            return None;
        }
    }
    Some(core::slice::from_raw_parts(p, len).to_vec())
}

/// One-time warning that HTTPS currently skips certificate verification.
static TLS_NOVERIFY_WARNED: AtomicBool = AtomicBool::new(false);

fn warn_tls_noverify() {
    if !TLS_NOVERIFY_WARNED.swap(true, Ordering::Relaxed) {
        ffi_kernel::klog_static(
            ffi_kernel::LOG_WARN,
            b"HTTPS: cert chain verification disabled for this request (cact_crypto has no cert signature support yet)\0",
        );
    }
}

fn do_fetch(
    url_bytes: &[u8],
    method: c_int,
    extra_headers: Option<&[u8]>,
    body_bytes: Option<&[u8]>,
    verify_tls: bool,
    out_buf: *mut c_void,
    out_len: u32,
) -> (c_int, HttpResp) {
    let mut resp = HttpResp {
        tls: 0,
        ..Default::default()
    };
    let parsed = match parse_url(url_bytes) {
        Some(u) => u,
        None => return (-1, resp),
    };
    resp.tls = parsed.tls as u8;

    let deadline = now_ticks().saturating_add(HTTP_TIMEOUT_TICKS);

    let Some(ip) = dns_resolve::resolve_a(&parsed.host) else {
        return (-1, resp);
    };

    let sock = tcp::tcp_socket();
    if sock < 0 {
        return (-1, resp);
    }

    if tcp::tcp_connect(sock, ip, parsed.port) != 0 {
        let _ = tcp::tcp_close(sock);
        return (-1, resp);
    }
    if !wait_established(sock, deadline) {
        resp.timed_out = 1;
        let _ = tcp::tcp_close(sock);
        return (-1, resp);
    }

    let request = match build_request(method, &parsed.host, &parsed.path, extra_headers, body_bytes) {
        Some(r) => r,
        None => {
            let _ = tcp::tcp_close(sock);
            return (-1, resp);
        }
    };

    let ret = if parsed.tls {
        if !verify_tls {
            warn_tls_noverify();
        }
        let Ok(name) = core::str::from_utf8(&parsed.host) else {
            let _ = tcp::tcp_close(sock);
            return (-1, resp);
        };
        match tls::tls_stream_open(sock, name, !verify_tls) {
            Some(mut stream) => {
                if tls::tls_stream_write(&mut stream, &request).is_err() {
                    let _ = tcp::tcp_close(sock);
                    return (-1, resp);
                }
                let mut src = PullSrc::Tls(&mut stream);
                do_request(&mut src, deadline, out_buf, out_len, &mut resp)
            }
            None => {
                let _ = tcp::tcp_close(sock);
                return (-1, resp);
            }
        }
    } else {
        if !sock_write(sock, &request) {
            resp.timed_out = 1;
            let _ = tcp::tcp_close(sock);
            return (-1, resp);
        }
        let mut src = PullSrc::Plain(sock);
        do_request(&mut src, deadline, out_buf, out_len, &mut resp)
    };

    let _ = tcp::tcp_close(sock);
    (ret, resp)
}

/// Perform an HTTP(S) request.
///
/// * `url` — `http://host[:port]/path` or `https://host[:port]/path` (IPv4
///   literals and bare `host/path` accepted; host must be resolvable via DNS).
/// * `method` — one of `CACT_HTTP_GET/POST/PUT/DELETE/HEAD`.
/// * `headers` — optional extra request headers, CRLF-separated (may be NULL).
/// * `body`/`body_len` — optional request body (POST/PUT).
/// * `flags` — `CACT_HTTP_FLAG_VERIFY_TLS` to require a verified TLS chain;
///   without it, HTTPS skips certificate verification (see module docs).
/// * `out_buf`/`out_len` — destination for the full response (headers + body).
///
/// Returns 0 and fills `*out` on success, -1 on transport/DNS/TLS failure,
/// -2 when `out_len` is too small to even hold the response headers.
#[no_mangle]
pub extern "C" fn cact_http_request(
    out: *mut HttpResp,
    url: *const c_char,
    method: c_int,
    headers: *const c_char,
    body: *const c_void,
    body_len: u32,
    flags: u32,
    out_buf: *mut c_void,
    out_len: u32,
) -> c_int {
    if out.is_null() || url.is_null() || out_buf.is_null() || out_len == 0 {
        return -1;
    }
    if !unsafe { stack::STACK_READY } {
        return -1;
    }
    // SAFETY: pointers validated above; cstr_bytes bounds the scan.
    let Some(url_bytes) = (unsafe { cstr_bytes(url) }) else {
        return -1;
    };
    let extra_headers = if headers.is_null() {
        None
    } else {
        // SAFETY: bounded NUL-terminated string.
        unsafe { cstr_bytes(headers) }.filter(|h| !h.is_empty())
    };
    let body_bytes = if body.is_null() || body_len == 0 {
        None
    } else {
        // SAFETY: caller promises body points to body_len readable bytes.
        Some(unsafe { slice::from_raw_parts(body.cast::<u8>(), body_len as usize) })
    };

    let (rc, resp) = do_fetch(
        &url_bytes,
        method,
        extra_headers.as_deref(),
        body_bytes,
        flags & CACT_HTTP_FLAG_VERIFY_TLS != 0,
        out_buf,
        out_len,
    );
    // SAFETY: out was validated non-null.
    unsafe {
        *out = resp;
    }
    rc
}

/// Convenience: HTTP GET.
#[no_mangle]
pub extern "C" fn cact_http_get(
    out: *mut HttpResp,
    url: *const c_char,
    headers: *const c_char,
    flags: u32,
    out_buf: *mut c_void,
    out_len: u32,
) -> c_int {
    cact_http_request(
        out,
        url,
        HTTP_GET,
        headers,
        core::ptr::null(),
        0,
        flags,
        out_buf,
        out_len,
    )
}

/// Convenience: HTTP POST with a request body.
#[no_mangle]
pub extern "C" fn cact_http_post(
    out: *mut HttpResp,
    url: *const c_char,
    headers: *const c_char,
    body: *const c_void,
    body_len: u32,
    flags: u32,
    out_buf: *mut c_void,
    out_len: u32,
) -> c_int {
    cact_http_request(
        out,
        url,
        HTTP_POST,
        headers,
        body,
        body_len,
        flags,
        out_buf,
        out_len,
    )
}
