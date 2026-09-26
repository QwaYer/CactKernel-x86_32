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
/// # Safety
///
/// The caller must be the only user of the UDP tables — running under the stack
/// lock, or in `stack_teardown` with `STACK_READY` cleared — because this reads
/// and advances the shared `NEXT_EPHEMERAL` counter and scans `udp_socks`.
unsafe fn pick_ephemeral() -> u16 {
    for _ in 0..(65535 - 49152 + 1) {
        // SAFETY: the caller contract (see # Safety) excludes any concurrent
        // access to the UDP statics, and `NEXT_EPHEMERAL` is a plain `u16`, so
        // this read cannot race.
        let p = unsafe { NEXT_EPHEMERAL };
        let next = if p == u16::MAX { 49152 } else { p + 1 };
        // SAFETY: the advance happens under the caller's exclusive access.
        unsafe { NEXT_EPHEMERAL = next };
        let mut taken = false;
        // SAFETY: `udp_socks` is the kernel-lifetime slot array and the caller
        // contract makes this borrow exclusive; the scan walks only the
        // fixed-length inline array.
        for s in (unsafe { &mut *core::ptr::addr_of_mut!(udp_socks) }).iter_mut() {
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

/// # Safety
///
/// The caller must be tearing the stack down (`stack_teardown`) while holding
/// `STACK_LOCK`, with `STACK_READY` already cleared, so no other CPU or task can
/// be using the UDP slots or their smoltcp sockets.
pub(crate) unsafe fn reset_udp_smoltcp_state() {
    // SAFETY: the caller contract (see # Safety) excludes every other user of
    // these statics for the duration of the call, so the writes below are the
    // only ones in flight.
    // SAFETY: the caller contract (see # Safety) excludes every other user of
    // these statics for the duration of the call, so this write is the only one
    // in flight.
    unsafe { UDP_HANDLE = [None; UDP_SOCK_MAX] };
    // SAFETY: as above.
    unsafe { NEXT_EPHEMERAL = 49152 };
    // SAFETY: as above.
    unsafe { UDP_PEER = [(0, 0); UDP_SOCK_MAX] };
    // SAFETY: as above; the borrow ends when this function returns.
    let udp = unsafe { &mut *core::ptr::addr_of_mut!(udp_socks) };
    {
        for s in udp.iter_mut() {
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
}

fn ensure_udp_bound(idx: usize, socks: &mut SocketSet<'static>) {
    // SAFETY: `idx < UDP_SOCK_MAX` is required by every caller (each one
    // bounds-checks its slot index first), so `UDP_HANDLE[idx]` is in bounds; all
    // accesses run under the stack lock, so no other task can be mutating the
    // slot at the same time.
    let Some(h) = (unsafe { UDP_HANDLE[idx] }) else {
        return;
    };
    // SAFETY: as `UDP_HANDLE[idx]` above — `idx` is in bounds and the slot is
    // only mutated under the stack lock this call holds.
    let port = unsafe { udp_socks[idx].local_port };
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
    // SAFETY: as above, for the slot's local-address field.
    let local = unsafe { udp_socks[idx].local_ip };
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

pub fn sync_udp_pcbs_from_smoltcp(socks: &mut SocketSet<'static>) {
    for idx in 0..UDP_SOCK_MAX {
        // SAFETY: called from `stack_poll` under `STACK_LOCK`, the same lock
        // every other user of `udp_socks`/`UDP_HANDLE` takes, and `idx` is
        // bounded by `UDP_SOCK_MAX`, so this slot read is in bounds and
        // uncontented.
        if unsafe { udp_socks[idx].used } == 0 {
            continue;
        }
        ensure_udp_bound(idx, socks);
        // SAFETY: as above, for the handle table.
        let Some(h) = (unsafe { UDP_HANDLE[idx] }) else {
            continue;
        };
        let s = socks.get_mut::<udp::Socket>(h);
        // SAFETY: as above, for the slot's readiness flag.
        unsafe { udp_socks[idx].rx_ready = if s.can_recv() { 1 } else { 0 } };
    }
}

#[no_mangle]
pub extern "C" fn udp_sock_alloc() -> i32 {
    // SAFETY: `STACK_READY` is a `static mut bool` written by `stack_init`/
    // `stack_teardown`; a byte load cannot tear, and reading it as false makes
    // this call fail closed.
    if !unsafe { stack::STACK_READY } {
        return -1;
    }
    let r = stack::with_iface_sockets(|_iface, socks| {
        // `with_iface_sockets` runs this closure under `STACK_LOCK`, the lock
        // every other path takes before touching `udp_socks`/`UDP_HANDLE`, and
        // `i` is a loop index bounded by `UDP_SOCK_MAX`.
        {
            for i in 0..UDP_SOCK_MAX {
                // SAFETY: `i` is in bounds, so this slot read is in bounds and
                // uncontented under the stack lock.
                if unsafe { udp_socks[i].used } != 0 {
                    continue;
                }
                // SAFETY: `i` is in bounds; `UDP_RX_META` is a kernel-lifetime
                // packet-metadata static handed to smoltcp only through this
                // socket, under the stack lock.
                let rx_meta = unsafe { &mut UDP_RX_META[i][..] };
                // SAFETY: as above, for `UDP_RX_PAY`.
                let rx_pay = unsafe { &mut UDP_RX_PAY[i][..] };
                let rx = udp::PacketBuffer::new(rx_meta, rx_pay);
                // SAFETY: as above, for `UDP_TX_META`.
                let tx_meta = unsafe { &mut UDP_TX_META[i][..] };
                // SAFETY: as above, for `UDP_TX_PAY`.
                let tx_pay = unsafe { &mut UDP_TX_PAY[i][..] };
                let tx = udp::PacketBuffer::new(tx_meta, tx_pay);
                let u = udp::Socket::new(rx, tx);
                let h = socks.add(u);
                // SAFETY: `UDP_HANDLE` is written under the stack lock, as every
                // access is.
                unsafe { UDP_HANDLE[i] = Some(h) };
                // SAFETY: as above, for the slot's fields.
                unsafe {
                    let slot = &mut udp_socks[i];
                    slot.used = 1;
                    slot.local_port = 0;
                    slot.local_ip = 0;
                    slot.rx_ready = 0;
                    slot.rx_len = 0;
                }
                return i as i32;
            }
            -1
        }
    });
    r.unwrap_or(-1)
}

#[no_mangle]
pub extern "C" fn udp_sock_free(idx: i32) {
    if idx < 0 || idx as usize >= UDP_SOCK_MAX {
        return;
    }
    let i = idx as usize;
    // SAFETY: `i` is bounds-checked above, so the `UDP_HANDLE[i]`,
    // `udp_socks[i]` and `UDP_PEER[i]` accesses are all in bounds.  The handle
    // is removed from the table *before* the smoltcp socket is unlinked, and a
    // slot is only ever freed by the task that owns its file description, so
    // there is no concurrent mutation of this row.
    // SAFETY: `i` is bounds-checked above, so the `UDP_HANDLE[i]` read is in
    // bounds; a slot is only ever freed by the task that owns its file
    // description, so there is no concurrent mutation of this row.
    let old = unsafe { UDP_HANDLE[i] };
    // SAFETY: as above — the handle is cleared before the smoltcp socket is
    // unlinked.
    unsafe { UDP_HANDLE[i] = None };
    if let Some(h) = old {
        let _ = stack::with_iface_sockets(|_iface, socks| {
            let _ = socks.remove(h);
        });
    }
    // SAFETY: as above, for the slot's `used` flag.
    unsafe { udp_socks[i].used = 0 };
    // SAFETY: as above, for the private peer table.
    unsafe { UDP_PEER[i] = (0, 0) };
}

/// `connect()` on a datagram socket.  UDP is connectionless, so this only
/// records the peer (validated here); `write()` afterwards sends one datagram
/// to it, and reads are still accepted from anyone.
#[no_mangle]
pub extern "C" fn udp_sock_connect(idx: i32, dst_ip: u32, dst_port: u16) -> i32 {
    if idx < 0 || idx as usize >= UDP_SOCK_MAX || dst_ip == 0 || dst_port == 0 {
        return -1;
    }
    // SAFETY: `idx` is bounds-checked above, so this slot read is in bounds; the
    // slot is only touched from socket paths running under the stack lock.
    if unsafe { udp_socks[idx as usize].used } == 0 {
        return -1;
    }
    // SAFETY: as above, for the private peer table write.
    unsafe { UDP_PEER[idx as usize] = (dst_ip, dst_port) };
    0
}

/// Peer recorded by [`udp_sock_connect`], or (0, 0) when unconnected.
pub(crate) fn udp_sock_peer(idx: i32) -> (u32, u16) {
    if idx < 0 || idx as usize >= UDP_SOCK_MAX {
        return (0, 0);
    }
    // SAFETY: `idx` is bounds-checked above, so the `UDP_PEER` read is in
    // bounds; the element is a plain `(u32, u16)` so the read cannot tear.
    unsafe { UDP_PEER[idx as usize] }
}

/// True when a datagram would find room in this socket's TX buffer right now —
/// the honest POLLOUT predicate for `sendto`/`write`, matching `tcp_can_send`.
pub(crate) fn udp_can_send(idx: usize) -> bool {
    if idx >= UDP_SOCK_MAX {
        return false;
    }
    let r = stack::with_iface_sockets(|_iface, socks| {
        // SAFETY: `idx < UDP_SOCK_MAX` is checked above, so `UDP_HANDLE[idx]` is
        // in bounds; the handle is an `Option<SocketHandle>` (Copy), so the read
        // cannot tear, and the socket is resolved through this closure's own
        // `SocketSet` under the stack lock.
        unsafe {
            let Some(h) = UDP_HANDLE[idx] else {
                return false;
            };
            socks.get_mut::<udp::Socket>(h).can_send()
        }
    });
    r.unwrap_or(false)
}

#[no_mangle]
pub extern "C" fn udp_sock_find_by_port(port: u16) -> *mut UdpSock {
    // SAFETY: the scan walks only the fixed-length inline array `udp_socks`, so
    // it stays in bounds; the returned pointer refers to a kernel-lifetime
    // static that the caller only uses until the slot is freed.
    unsafe {
        for s in (*core::ptr::addr_of_mut!(udp_socks)).iter_mut() {
            if s.used != 0 && s.local_port == port {
                return s as *mut UdpSock;
            }
        }
    }
    core::ptr::null_mut()
}

/// Receive one datagram.
///
/// # Safety
///
/// `buf` may be null (the call then returns -1), but if non-null it must point
/// to `max_len` writable bytes that stay live for the call.  `src_ip_out` and
/// `src_port_out` may be null (that part of the result is dropped) but, if
/// non-null, must point to a writable, aligned `u32`/`u16` respectively.
#[no_mangle]
pub unsafe extern "C" fn udp_sock_recv(
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
    let r = stack::with_iface_sockets(|_iface, socks| {
        // `with_iface_sockets` holds `STACK_LOCK` for the whole closure, so no
        // other path can touch `udp_socks`/`UDP_HANDLE`/this socket, and `i` is
        // bounds-checked above.
        {
            ensure_udp_bound(i, socks);
            // SAFETY: `UDP_HANDLE[i]` is read under the stack lock this closure
            // holds, and `i` is in bounds.
            let Some(h) = (unsafe { UDP_HANDLE[i] }) else {
                return -1;
            };
            let s = socks.get_mut::<udp::Socket>(h);
            let Ok((data, meta)) = s.recv() else {
                return 0;
            };
            let to_copy = core::cmp::min(data.len(), max_len as usize);
            // SAFETY: the caller contract (see # Safety) makes `buf` a
            // `max_len`-byte buffer, and `to_copy` is clamped to `max_len`, so the
            // copy stays inside it; `data` is the received payload.
            unsafe { core::ptr::copy_nonoverlapping(data.as_ptr(), buf, to_copy) };
            let ep = meta.endpoint;
            if !src_ip_out.is_null() {
                let IpAddress::Ipv4(a) = ep.addr;
                let o = a.octets();
                // SAFETY: the caller contract makes `src_ip_out`, when non-null, a
                // writable, aligned `u32`; the null check above excluded null.
                unsafe {
                    *src_ip_out = u32::from(o[0]) << 24
                        | u32::from(o[1]) << 16
                        | u32::from(o[2]) << 8
                        | u32::from(o[3]);
                }
            }
            if !src_port_out.is_null() {
                // SAFETY: as above, for `src_port_out`.
                unsafe { *src_port_out = ep.port };
            }
            // SAFETY: the slot's readiness flag is only written under the stack
            // lock this closure holds.
            unsafe { udp_socks[i].rx_ready = if s.can_recv() { 1 } else { 0 } };
            to_copy as i32
        }
    });
    r.unwrap_or(-1)
}

/// Send one datagram.
///
/// # Safety
///
/// `data` may be null (the call then returns -1), but if non-null it must point
/// to `len` readable bytes that stay live for the duration of the call.
#[no_mangle]
pub unsafe extern "C" fn udp_sock_send(
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
    let r = stack::with_iface_sockets(|_iface, socks| {
        // `with_iface_sockets` holds `STACK_LOCK` for the whole closure, so the
        // `udp_socks`/`UDP_HANDLE` accesses and `pick_ephemeral` are serialized
        // against every other UDP path, and `i` is bounds-checked above.
        {
            // SAFETY: `i` is in bounds and the closure holds the stack lock, so
            // this slot read is in bounds and uncontented.
            if unsafe { udp_socks[i].used } == 0 {
                return -1;
            }
            // smoltcp refuses to transmit from a socket whose local port is still 0
            // (SendError::Unaddressable), so an unbound socket gets an ephemeral
            // port here — at first send, not earlier: an explicit bind() must win,
            // and sync_udp_pcbs_from_smoltcp runs long before the app can bind().
            // SAFETY: as above, for the slot's local-port field.
            if unsafe { udp_socks[i].local_port } == 0 {
                // SAFETY: `pick_ephemeral` needs its caller to be the only user of
                // the UDP tables, which is this closure under the stack lock.
                let p = unsafe { pick_ephemeral() };
                if p == 0 {
                    return -1;
                }
                // SAFETY: as above, for the slot's local-port field.
                unsafe { udp_socks[i].local_port = p };
            }
            ensure_udp_bound(i, socks);
            // SAFETY: as above, for the handle table.
            let Some(h) = (unsafe { UDP_HANDLE[i] }) else {
                return -1;
            };
            let s = socks.get_mut::<udp::Socket>(h);
            // SAFETY: the caller contract (see # Safety) makes `data` a `len`-byte
            // readable buffer, which is what `from_raw_parts` asserts.
            let slice = unsafe { core::slice::from_raw_parts(data, len as usize) };
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
    let r = stack::with_iface_sockets(|_iface, socks| {
        // SAFETY: `idx < UDP_SOCK_MAX` is checked above, so this slot read is in
        // bounds; it is plain Copy data whose read cannot tear, and the socket is
        // reached through this closure's own `SocketSet` under `STACK_LOCK`.
        if unsafe { udp_socks[idx].used } == 0 {
            return (false, true);
        }
        // SAFETY: as above, for the handle table.
        let Some(h) = (unsafe { UDP_HANDLE[idx] }) else {
            return (false, true);
        };
        (socks.get_mut::<udp::Socket>(h).can_recv(), false)
    });
    r.unwrap_or((false, true))
}
