//! UDP sockets implemented on top of smoltcp.

use smoltcp::iface::{SocketHandle, SocketSet};
use smoltcp::socket::udp;
use smoltcp::wire::{IpAddress, IpListenEndpoint};

use crate::stack::{self};
use crate::types::{UdpSock, UDP_SOCK_MAX};

#[no_mangle]
pub static mut udp_socks: [UdpSock; UDP_SOCK_MAX] = [UdpSock {
    used: 0,
    local_port: 0,
    local_ip: 0,
    rx_buf: [0; crate::types::UDP_RX_BUF_SIZE],
    rx_len: 0,
    rx_ready: 0,
    last_src_ip: 0,
    last_src_port: 0,
}; UDP_SOCK_MAX];

static mut UDP_RX_META: [[udp::PacketMetadata; 4]; UDP_SOCK_MAX] =
    [[udp::PacketMetadata::EMPTY; 4]; UDP_SOCK_MAX];
static mut UDP_RX_PAY: [[u8; 2048]; UDP_SOCK_MAX] = [[0; 2048]; UDP_SOCK_MAX];
static mut UDP_TX_META: [[udp::PacketMetadata; 4]; UDP_SOCK_MAX] =
    [[udp::PacketMetadata::EMPTY; 4]; UDP_SOCK_MAX];
static mut UDP_TX_PAY: [[u8; 2048]; UDP_SOCK_MAX] = [[0; 2048]; UDP_SOCK_MAX];
static mut UDP_HANDLE: [Option<SocketHandle>; UDP_SOCK_MAX] = [None; UDP_SOCK_MAX];

/// Next local port to hand to a socket that was never bind()ed.
static mut NEXT_EPHEMERAL: u16 = 49152;

/// Peer recorded by `connect()` on a datagram socket; (0, 0) = unconnected.
/// Kept out of `UdpSock`/`Ksock` on purpose: those are C-visible mirrors, and
/// the peer is private to this crate.
static mut UDP_PEER: [(u32, u16); UDP_SOCK_MAX] = [(0, 0); UDP_SOCK_MAX];

/// Pick a local port not already held by another UDP socket.
unsafe fn pick_ephemeral() -> u16 {
    for _ in 0..(65535 - 49152 + 1) {
        let p = NEXT_EPHEMERAL;
        NEXT_EPHEMERAL = if p >= 65535 { 49152 } else { p + 1 };
        let mut taken = false;
        for s in udp_socks.iter_mut() {
            if s.used != 0 && s.local_port == p {
                taken = true;
                break;
            }
        }
        if !taken {
            return p;
        }
    }
    0
}

pub(crate) unsafe fn reset_udp_smoltcp_state() {
    UDP_HANDLE = [None; UDP_SOCK_MAX];
    NEXT_EPHEMERAL = 49152;
    UDP_PEER = [(0, 0); UDP_SOCK_MAX];
    for s in udp_socks.iter_mut() {
        *s = UdpSock {
            used: 0,
            local_port: 0,
            local_ip: 0,
            rx_buf: [0; crate::types::UDP_RX_BUF_SIZE],
            rx_len: 0,
            rx_ready: 0,
            last_src_ip: 0,
            last_src_port: 0,
        };
    }
}

fn ensure_udp_bound(idx: usize, socks: &mut SocketSet<'static>) {
    unsafe {
        let h = match UDP_HANDLE[idx] {
            Some(h) => h,
            None => return,
        };
        let port = udp_socks[idx].local_port;
        if port == 0 {
            return;
        }
        let s = socks.get_mut::<udp::Socket>(h);
        if s.is_open() {
            return;
        }
        // A bind() may name a local address as well as a port (host order, 0 =
        // INADDR_ANY).  Honour it: without the address the socket answers on
        // whatever address the packet arrived on, which is not what bind(2)
        // promises.
        let local = udp_socks[idx].local_ip;
        let ep = IpListenEndpoint {
            addr: if local != 0 {
                Some(IpAddress::Ipv4(crate::stack::ipv4_from_host(local)))
            } else {
                None
            },
            port,
        };
        let _ = s.bind(ep);
    }
}

pub fn sync_udp_pcbs_from_smoltcp(socks: &mut SocketSet<'static>) {
    for idx in 0..UDP_SOCK_MAX {
        unsafe {
            if udp_socks[idx].used == 0 {
                continue;
            }
            ensure_udp_bound(idx, socks);
            let Some(h) = UDP_HANDLE[idx] else {
                continue;
            };
            let s = socks.get_mut::<udp::Socket>(h);
            udp_socks[idx].rx_ready = if s.can_recv() { 1 } else { 0 };
        }
    }
}

#[no_mangle]
pub extern "C" fn udp_sock_alloc() -> i32 {
    if !unsafe { stack::STACK_READY } {
        return -1;
    }
    let r = stack::with_iface_sockets(|_iface, socks| unsafe {
        for i in 0..UDP_SOCK_MAX {
            if udp_socks[i].used == 0 {
                let rx = udp::PacketBuffer::new(&mut UDP_RX_META[i][..], &mut UDP_RX_PAY[i][..]);
                let tx = udp::PacketBuffer::new(&mut UDP_TX_META[i][..], &mut UDP_TX_PAY[i][..]);
                let u = udp::Socket::new(rx, tx);
                let h = socks.add(u);
                UDP_HANDLE[i] = Some(h);
                udp_socks[i].used = 1;
                udp_socks[i].local_port = 0;
                udp_socks[i].local_ip = 0;
                udp_socks[i].rx_ready = 0;
                udp_socks[i].rx_len = 0;
                return i as i32;
            }
        }
        -1
    });
    r.unwrap_or(-1)
}

#[no_mangle]
pub extern "C" fn udp_sock_free(idx: i32) {
    if idx < 0 || idx as usize >= UDP_SOCK_MAX {
        return;
    }
    let i = idx as usize;
    unsafe {
        if let Some(h) = UDP_HANDLE[i].take() {
            let _ = stack::with_iface_sockets(|_iface, socks| {
                let rm = socks.remove(h);
                core::mem::drop(rm);
            });
        }
        udp_socks[i].used = 0;
        UDP_PEER[i] = (0, 0);
    }
}

/// `connect()` on a datagram socket.  UDP is connectionless, so this only
/// records the peer (validated here); `write()` afterwards sends one datagram
/// to it, and reads are still accepted from anyone.
#[no_mangle]
pub extern "C" fn udp_sock_connect(idx: i32, dst_ip: u32, dst_port: u16) -> i32 {
    if idx < 0 || idx as usize >= UDP_SOCK_MAX || dst_ip == 0 || dst_port == 0 {
        return -1;
    }
    unsafe {
        if udp_socks[idx as usize].used == 0 {
            return -1;
        }
        UDP_PEER[idx as usize] = (dst_ip, dst_port);
    }
    0
}

/// Peer recorded by [`udp_sock_connect`], or (0, 0) when unconnected.
pub(crate) fn udp_sock_peer(idx: i32) -> (u32, u16) {
    if idx < 0 || idx as usize >= UDP_SOCK_MAX {
        return (0, 0);
    }
    unsafe { UDP_PEER[idx as usize] }
}

/// True when a datagram would find room in this socket's TX buffer right now —
/// the honest POLLOUT predicate for `sendto`/`write`, matching `tcp_can_send`.
pub(crate) fn udp_can_send(idx: usize) -> bool {
    if idx >= UDP_SOCK_MAX {
        return false;
    }
    let r = stack::with_iface_sockets(|_iface, socks| unsafe {
        let Some(h) = UDP_HANDLE[idx] else {
            return false;
        };
        socks.get_mut::<udp::Socket>(h).can_send()
    });
    r.unwrap_or(false)
}

#[no_mangle]
pub extern "C" fn udp_sock_find_by_port(port: u16) -> *mut UdpSock {
    unsafe {
        for s in udp_socks.iter_mut() {
            if s.used != 0 && s.local_port == port {
                return s as *mut UdpSock;
            }
        }
    }
    core::ptr::null_mut()
}

#[no_mangle]
pub extern "C" fn udp_sock_recv(
    idx: i32,
    buf: *mut u8,
    max_len: u16,
    src_ip_out: *mut u32,
    src_port_out: *mut u16,
) -> i32 {
    if idx < 0 || idx as usize >= UDP_SOCK_MAX || buf.is_null() {
        return -1;
    }
    let i = idx as usize;
    let r = stack::with_iface_sockets(|_iface, socks| unsafe {
        ensure_udp_bound(i, socks);
        let Some(h) = UDP_HANDLE[i] else {
            return -1;
        };
        let s = socks.get_mut::<udp::Socket>(h);
        let Ok((data, meta)) = s.recv() else {
            return 0;
        };
        let to_copy = core::cmp::min(data.len(), max_len as usize);
        core::ptr::copy_nonoverlapping(data.as_ptr(), buf, to_copy);
        let ep = meta.endpoint;
        if !src_ip_out.is_null() {
            let IpAddress::Ipv4(a) = ep.addr;
            let o = a.octets();
            *src_ip_out = u32::from(o[0]) << 24
                | u32::from(o[1]) << 16
                | u32::from(o[2]) << 8
                | u32::from(o[3]);
        }
        if !src_port_out.is_null() {
            *src_port_out = ep.port;
        }
        udp_socks[i].rx_ready = if s.can_recv() { 1 } else { 0 };
        to_copy as i32
    });
    r.unwrap_or(-1)
}

#[no_mangle]
pub extern "C" fn udp_sock_send(
    idx: i32,
    dst_ip: u32,
    dst_port: u16,
    data: *const u8,
    len: u16,
) -> i32 {
    if idx < 0 || idx as usize >= UDP_SOCK_MAX || data.is_null() {
        return -1;
    }
    let i = idx as usize;
    let r = stack::with_iface_sockets(|_iface, socks| unsafe {
        if udp_socks[i].used == 0 {
            return -1;
        }
        // smoltcp refuses to transmit from a socket whose local port is still 0
        // (SendError::Unaddressable), so an unbound socket gets an ephemeral
        // port here — at first send, not earlier: an explicit bind() must win,
        // and sync_udp_pcbs_from_smoltcp runs long before the app can bind().
        if udp_socks[i].local_port == 0 {
            let p = pick_ephemeral();
            if p == 0 {
                return -1;
            }
            udp_socks[i].local_port = p;
        }
        ensure_udp_bound(i, socks);
        let Some(h) = UDP_HANDLE[i] else {
            return -1;
        };
        let s = socks.get_mut::<udp::Socket>(h);
        let slice = core::slice::from_raw_parts(data, len as usize);
        let dst_a = core::net::Ipv4Addr::from_bits(dst_ip);
        // A datagram bigger than the socket's payload capacity can never be
        // queued, so that is a hard error rather than "retry later".
        if len as usize > s.payload_send_capacity() {
            return -1;
        }
        match s.send_slice(slice, (dst_a, dst_port)) {
            Ok(()) => len as i32,
            // Transient: the TX buffer still holds other datagrams.  0 means
            // "nothing queued, retry after POLLOUT" (same convention as
            // tcp_send); CACT_SOCKCTL_SENDTO turns it into -EAGAIN.
            Err(udp::SendError::BufferFull) => 0,
            Err(_) => -1,
        }
    });
    r.unwrap_or(-1)
}

/// Fresh readiness for `poll()`: `(datagram waiting, slot unusable)`.
///
/// `can_recv()` is whether a datagram is queued — the honest POLLIN predicate.
pub(crate) fn udp_poll_status(idx: usize) -> (bool, bool) {
    if idx >= UDP_SOCK_MAX {
        return (false, true);
    }
    let r = stack::with_iface_sockets(|_iface, socks| unsafe {
        if udp_socks[idx].used == 0 {
            return (false, true);
        }
        let Some(h) = UDP_HANDLE[idx] else {
            return (false, true);
        };
        (socks.get_mut::<udp::Socket>(h).can_recv(), false)
    });
    r.unwrap_or((false, true))
}
