//! Blocking DNS A-record resolution over UDP port 53 using a dedicated smoltcp UDP socket.

use core::ffi::{c_char, c_int};
use core::net::Ipv4Addr;

use smoltcp::iface::{SocketHandle, SocketSet};
use smoltcp::socket::udp;
use smoltcp::wire::IpAddress;

use crate::config;
use crate::ffi_kernel;
use crate::stack;

const DNS_PORT: u16 = 53;
const QTYPE_A: u16 = 1;
const QCLASS_IN: u16 = 1;
const DNS_TIMEOUT_TICKS: u32 = 300;
const POLL_SLICE_TICKS: u32 = 2;

static mut DNS_RX_META: [udp::PacketMetadata; 4] = [udp::PacketMetadata::EMPTY; 4];
static mut DNS_RX_BUF: [u8; 2048] = [0; 2048];
static mut DNS_TX_META: [udp::PacketMetadata; 4] = [udp::PacketMetadata::EMPTY; 4];
static mut DNS_TX_BUF: [u8; 2048] = [0; 2048];
static mut DNS_HANDLE: Option<SocketHandle> = None;

pub unsafe fn init_socket(socks: &mut SocketSet<'static>) {
    if DNS_HANDLE.is_some() {
        return;
    }
    let rx = udp::PacketBuffer::new(&mut DNS_RX_META[..], &mut DNS_RX_BUF[..]);
    let tx = udp::PacketBuffer::new(&mut DNS_TX_META[..], &mut DNS_TX_BUF[..]);
    let u = udp::Socket::new(rx, tx);
    DNS_HANDLE = Some(socks.add(u));
}

pub unsafe fn remove_socket(socks: &mut SocketSet<'static>) {
    if let Some(h) = DNS_HANDLE.take() {
        let _ = socks.remove(h);
    }
}

fn ipv4_host_from_smoltcp_v4(a: smoltcp::wire::Ipv4Address) -> u32 {
    let o = a.octets();
    u32::from(o[0]) << 24 | u32::from(o[1]) << 16 | u32::from(o[2]) << 8 | u32::from(o[3])
}

fn parse_ipv4_literal(host: &[u8]) -> Option<u32> {
    let s = core::str::from_utf8(host).ok()?;
    let mut parts = s.split('.');
    let a: u32 = parts.next()?.parse().ok()?;
    let b: u32 = parts.next()?.parse().ok()?;
    let c: u32 = parts.next()?.parse().ok()?;
    let d: u32 = parts.next()?.parse().ok()?;
    if parts.next().is_some() {
        return None;
    }
    if a > 255 || b > 255 || c > 255 || d > 255 {
        return None;
    }
    Some((a << 24) | (b << 16) | (c << 8) | d)
}

fn read_u16_be(pkt: &[u8], i: usize) -> Option<u16> {
    Some(u16::from_be_bytes([*pkt.get(i)?, *pkt.get(i + 1)?]))
}

fn read_u32_be(pkt: &[u8], i: usize) -> Option<u32> {
    Some(u32::from_be_bytes([
        *pkt.get(i)?,
        *pkt.get(i + 1)?,
        *pkt.get(i + 2)?,
        *pkt.get(i + 3)?,
    ]))
}

fn skip_name(pkt: &[u8], mut i: usize) -> Option<usize> {
    loop {
        let l = *pkt.get(i)? as usize;
        if l == 0 {
            return Some(i + 1);
        }
        if (l & 0xc0) == 0xc0 {
            return Some(i + 2);
        }
        if l > 63 {
            return None;
        }
        i = i.checked_add(1)?.checked_add(l)?;
    }
}

fn encode_qname(out: &mut [u8], host: &str) -> Option<usize> {
    let mut w = 0usize;
    if host.is_empty() || host.len() > 253 {
        return None;
    }
    for label in host.split('.') {
        if label.is_empty() {
            continue;
        }
        let b = label.as_bytes();
        if b.is_empty() || b.len() > 63 {
            return None;
        }
        if w + 1 + b.len() > out.len() {
            return None;
        }
        out[w] = b.len() as u8;
        w += 1;
        out[w..w + b.len()].copy_from_slice(b);
        w += b.len();
    }
    if w >= out.len() {
        return None;
    }
    out[w] = 0;
    w += 1;
    Some(w)
}

fn flush_dns_recv(socks: &mut SocketSet<'static>) {
    let Some(h) = (unsafe { DNS_HANDLE }) else {
        return;
    };
    let s = socks.get_mut::<udp::Socket>(h);
    while s.recv().is_ok() {}
}

fn pick_ephemeral_port() -> u16 {
    let t = unsafe { ffi_kernel::timer_ticks_get() };
    0xc000u16 | (t as u16 & 0x3fff)
}

fn parse_dns_a_response(pkt: &[u8], expect_id: u16) -> Option<u32> {
    if pkt.len() < 12 {
        return None;
    }
    let id = read_u16_be(pkt, 0)?;
    if id != expect_id {
        return None;
    }
    let flags = read_u16_be(pkt, 2)?;
    if (flags & 0x8000) == 0 {
        return None;
    }
    let rcode = flags & 0x000f;
    if rcode != 0 {
        return None;
    }
    let qd = read_u16_be(pkt, 4)? as usize;
    let an = read_u16_be(pkt, 6)? as usize;
    if qd == 0 || an == 0 {
        return None;
    }
    let mut pos = 12usize;
    for _ in 0..qd {
        pos = skip_name(pkt, pos)?;
        pos = pos.checked_add(4)?;
    }
    for _ in 0..an {
        pos = skip_name(pkt, pos)?;
        let typ = read_u16_be(pkt, pos)?;
        pos += 2;
        let class = read_u16_be(pkt, pos)?;
        pos += 2;
        pos = pos.checked_add(4)?;
        let rdlen = read_u16_be(pkt, pos)? as usize;
        pos += 2;
        if typ == QTYPE_A && class == QCLASS_IN && rdlen == 4 {
            return read_u32_be(pkt, pos);
        }
        pos = pos.checked_add(rdlen)?;
    }
    None
}

fn try_recv_dns_reply(socks: &mut SocketSet<'static>, dns_host: u32, id: u16) -> Option<u32> {
    let h = unsafe { DNS_HANDLE }?;
    let s = socks.get_mut::<udp::Socket>(h);
    let (data, meta) = s.recv().ok()?;
    let IpAddress::Ipv4(src_v4) = meta.endpoint.addr;
    if ipv4_host_from_smoltcp_v4(src_v4) != dns_host {
        return None;
    }
    if meta.endpoint.port != DNS_PORT {
        return None;
    }
    parse_dns_a_response(data, id)
}

fn resolve_once(name: &str, dns_host: u32, query_buf: &mut [u8]) -> Option<u32> {
    let dns_ip = Ipv4Addr::from_bits(dns_host);
    let id = (unsafe { ffi_kernel::timer_ticks_get() } as u16) ^ 0xa5a5;
    let mut w = 0usize;
    query_buf.get_mut(w..w + 2)?.copy_from_slice(&id.to_be_bytes());
    w += 2;
    query_buf.get_mut(w..w + 2)?.copy_from_slice(&0x0100u16.to_be_bytes());
    w += 2;
    query_buf.get_mut(w..w + 2)?.copy_from_slice(&1u16.to_be_bytes());
    w += 2;
    query_buf.get_mut(w..w + 2)?.copy_from_slice(&0u16.to_be_bytes());
    w += 2;
    query_buf.get_mut(w..w + 2)?.copy_from_slice(&0u16.to_be_bytes());
    w += 2;
    query_buf.get_mut(w..w + 2)?.copy_from_slice(&0u16.to_be_bytes());
    w += 2;
    let qn_end = encode_qname(&mut query_buf[w..], name)?;
    w += qn_end;
    query_buf.get_mut(w..w + 2)?.copy_from_slice(&QTYPE_A.to_be_bytes());
    w += 2;
    query_buf.get_mut(w..w + 2)?.copy_from_slice(&QCLASS_IN.to_be_bytes());
    w += 2;
    let qlen = w;

    stack::with_iface_sockets(|_iface, socks| {
        let h = unsafe { DNS_HANDLE }?;
        flush_dns_recv(socks);
        let s = socks.get_mut::<udp::Socket>(h);
        if !s.is_open() {
            let p = pick_ephemeral_port();
            s.bind(p).ok()?;
        }
        s.send_slice(&query_buf[..qlen], (dns_ip, DNS_PORT)).ok()?;
        Some(())
    })?;

    let deadline = unsafe { ffi_kernel::timer_ticks_get() }.saturating_add(DNS_TIMEOUT_TICKS);
    let mut next_wake = unsafe { ffi_kernel::timer_ticks_get() };
    while unsafe { ffi_kernel::timer_ticks_get() } < deadline {
        stack::stack_poll();
        let now = unsafe { ffi_kernel::timer_ticks_get() };
        if now >= next_wake {
            if let Some(ip) =
                stack::with_iface_sockets(|_iface, socks| try_recv_dns_reply(socks, dns_host, id))
                    .flatten()
            {
                return Some(ip);
            }
            next_wake = now.saturating_add(POLL_SLICE_TICKS);
        }
        unsafe { ffi_kernel::sched_sleep_ticks(1) };
    }
    None
}

#[no_mangle]
pub extern "C" fn rust_net_dns_resolve_a(name: *const c_char, out_ip_host: *mut u32) -> c_int {
    if name.is_null() || out_ip_host.is_null() {
        return -1;
    }
    if !unsafe { stack::STACK_READY } {
        return -1;
    }
    let mut len = 0usize;
    unsafe {
        while *name.add(len) != 0 {
            len += 1;
            if len > 253 {
                return -1;
            }
        }
    }
    let host = unsafe { core::slice::from_raw_parts(name.cast::<u8>(), len) };
    if let Some(ip) = parse_ipv4_literal(host) {
        unsafe {
            *out_ip_host = ip;
        }
        return 0;
    }
    let dns = config::dns_host();
    if dns == 0 {
        return -1;
    }
    if config::ip_host() == 0 || config::netmask_host() == 0 {
        return -1;
    }
    let Ok(name_str) = core::str::from_utf8(host) else {
        return -1;
    };
    let mut buf = [0u8; 512];
    match resolve_once(name_str, dns, &mut buf) {
        Some(ip) => {
            unsafe { *out_ip_host = ip };
            0
        }
        None => -1,
    }
}
