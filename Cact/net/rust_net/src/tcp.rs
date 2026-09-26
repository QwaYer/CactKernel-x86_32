//! TCP sockets on smoltcp; keeps `tcp_sockets[]` aligned with the C ABI (state, accept, select).

use core::net::Ipv4Addr;
use core::sync::atomic::{AtomicU32, Ordering};

use smoltcp::iface::{Interface, SocketHandle, SocketSet};
use smoltcp::socket::tcp;
use smoltcp::wire::IpAddress;

use crate::stack::{self};
use crate::types::*;

fn ipv4_u32(a: Ipv4Addr) -> u32 {
    let o = a.octets();
    u32::from(o[0]) << 24 | u32::from(o[1]) << 16 | u32::from(o[2]) << 8 | u32::from(o[3])
}

pub(crate) fn tcp_lock() {
    let flags: u32;
    // SAFETY: this runs on the current CPU.  `pushfd`/`pop` capture EFLAGS into
    // `flags` and the `nomem, preserves_flags` options assert that the asm reads
    // and writes no memory, so only the named register is affected.
    unsafe {
        core::arch::asm!("pushfd; pop {flags}", flags = out(reg) flags, options(nomem, preserves_flags));
    }
    // SAFETY: `cli` masks maskable interrupts on this CPU; it reads and writes no
    // memory and changes no architectural state but the interrupt flag.
    unsafe {
        core::arch::asm!("cli");
    }
    TCP_SAVED_FLAGS.store(flags, Ordering::Relaxed);
    while TCP_LOCK.compare_exchange(0, 1, Ordering::Acquire, Ordering::Relaxed).is_err() {
        // SAFETY: `pause` is a spin-loop hint; it reads and writes no memory and
        // changes no architectural state.
        unsafe { core::arch::asm!("pause"); }
    }
}

pub(crate) fn tcp_unlock() {
    let flags = TCP_SAVED_FLAGS.load(Ordering::Relaxed);
    TCP_LOCK.store(0, Ordering::Release);
    if flags & (1 << 9) != 0 {
        // SAFETY: `sti` only sets the interrupt flag on this CPU and touches no
        // memory.  It runs only when the matching `tcp_lock` recorded that
        // interrupts were enabled before the lock was taken.
        unsafe { core::arch::asm!("sti"); }
    }
}

static TCP_SAVED_FLAGS: AtomicU32 = AtomicU32::new(0);

#[no_mangle]
pub static mut tcp_sockets: [TcpSocket; TCP_MAX_SOCKETS] = [TcpSocket {
    used: 0,
    state: TCP_CLOSED,
    local_ip: 0,
    local_port: 0,
    remote_ip: 0,
    remote_port: 0,
    snd_una: 0,
    snd_nxt: 0,
    snd_wnd: 0,
    rcv_nxt: 0,
    rcv_wnd: TCP_RX_BUF_SIZE as u32,
    rx_buf: [0; TCP_RX_BUF_SIZE],
    rx_head: 0,
    rx_tail: 0,
    on_data: core::ptr::null_mut(),
    on_event: core::ptr::null_mut(),
    listen_parent: -1,
    accept_ready: 0,
    nodelay: 0,
    keepalive: 0,
}; TCP_MAX_SOCKETS];

pub(crate) static mut TCP_RX_BUFS: [[u8; TCP_RX_BUF_SIZE]; TCP_MAX_SOCKETS] = [[0; TCP_RX_BUF_SIZE]; TCP_MAX_SOCKETS];
pub(crate) static mut TCP_TX_BUFS: [[u8; TCP_RX_BUF_SIZE]; TCP_MAX_SOCKETS] = [[0; TCP_RX_BUF_SIZE]; TCP_MAX_SOCKETS];
pub(crate) static mut TCP_HANDLE: [Option<SocketHandle>; TCP_MAX_SOCKETS] = [None; TCP_MAX_SOCKETS];

pub(crate) static mut NEXT_EPHEMERAL: u16 = 49152;

static TCP_LOCK: AtomicU32 = AtomicU32::new(0);

/// Run `f` against the smoltcp TCP socket backing socket index `idx`, holding
/// `tcp_lock` (interrupts disabled) for the whole access so the poll thread's
/// `sync_tcp_pcbs_from_smoltcp` cannot race us on the socket state.
pub(crate) fn with_tcp_socket<R>(idx: i32, f: impl FnOnce(&mut smoltcp::socket::tcp::Socket) -> R) -> Option<R> {
    if idx < 0 || idx as usize >= TCP_MAX_SOCKETS {
        return None;
    }
    crate::stack::with_iface_sockets(|_iface, socks| {
        tcp_lock();
        // SAFETY: the index is bounds-checked above (`idx < TCP_MAX_SOCKETS`), so
        // indexing the copied `TCP_HANDLE` array is in bounds; every read/write
        // of `TCP_HANDLE` and of the smoltcp `SocketSet` socket happens between
        // `tcp_lock`/`tcp_unlock` (interrupts off), which the poll thread also
        // takes, so no other context can mutate the handle or the socket
        // concurrently.
        let h = unsafe { (*core::ptr::addr_of!(TCP_HANDLE))[idx as usize] };
        let r = h.map(|h| f(socks.get_mut::<smoltcp::socket::tcp::Socket>(h)));
        tcp_unlock();
        r
    })
    .flatten()
}

/// True when a `write()` on this socket would accept at least one byte right
/// now — the honest POLLOUT predicate (an ESTABLISHED socket whose TX buffer
/// or peer window is full is not writable).
pub(crate) fn tcp_can_send(idx: usize) -> bool {
    with_tcp_socket(idx as i32, |s| s.can_send()).unwrap_or(false)
}

/// Fresh readiness for `poll()`: `(byte buffered, inbound connection pending,
/// peer closed)`.
///
/// Queried from smoltcp rather than read out of the cached mirror so a
/// select()/poll() loop never acts on a state that is up to one poll period
/// old.  `None` means the slot has no smoltcp socket at all (closed or torn
/// down), which the caller reports as `POLLERR`.
///
/// `can_recv()` — not `may_recv()` — is the data predicate: `may_recv()` is
/// true for the whole life of an open connection, which made every established
/// socket look readable and turned select() loops into a spin.
pub(crate) fn tcp_poll_status(idx: usize) -> Option<(bool, bool, bool)> {
    if idx >= TCP_MAX_SOCKETS {
        return None;
    }
    tcp_lock();
    let (used, listen_parent) = {
        // SAFETY: `tcp_sockets` is read under `tcp_lock` (interrupts off) — the
        // same lock every writer takes — and `idx` is bounds-checked above, so
        // the borrow is in bounds and cannot race another context; it is consumed
        // by the two field copies.
        let s = unsafe { &tcp_sockets[idx] };
        (s.used, s.listen_parent)
    };
    tcp_unlock();
    if used == 0 {
        return None;
    }
    with_tcp_socket(idx as i32, |s| {
        let st = s.state();
        let eof = matches!(
            st,
            tcp::State::CloseWait | tcp::State::Closed | tcp::State::TimeWait
        );
        // A pending inbound connection.  `listen_endpoint().port != 0` alone is
        // not enough: an accepted child keeps the endpoint it was listening on,
        // so it would keep claiming to be accept-ready.  Children are the slots
        // with a listen_parent, which is exactly what excludes them here.
        let accept = st == tcp::State::Established
            && s.listen_endpoint().port != 0
            && listen_parent < 0;
        (s.can_recv(), accept, eof)
    })
}

/// # Safety
///
/// The caller must be tearing the stack down (`stack_teardown`) while holding
/// `STACK_LOCK`, with `STACK_READY` already cleared, so no other CPU or task can
/// be touching the TCP slots or their smoltcp sockets.
pub(crate) unsafe fn reset_tcp_smoltcp_state() {
    // SAFETY: the caller contract (see # Safety) excludes every other user of
    // these statics for the duration of the call, so the writes below are the
    // only accesses in flight.
    // SAFETY: the caller contract (see # Safety) excludes every other user of
    // these statics for the duration of the call, so this write is the only
    // access in flight.
    unsafe { TCP_HANDLE = [None; TCP_MAX_SOCKETS] };
    // SAFETY: as above.
    unsafe { NEXT_EPHEMERAL = 49152 };
    // SAFETY: as above; `tcp_sockets` is the kernel-lifetime slot array and the
    // borrow ends when this function returns.
    let sockets = unsafe { &mut *core::ptr::addr_of_mut!(tcp_sockets) };
    {
        for s in sockets.iter_mut() {
            *s = TcpSocket {
                used: 0,
                state: TCP_CLOSED,
                local_ip: 0,
                local_port: 0,
                remote_ip: 0,
                remote_port: 0,
                snd_una: 0,
                snd_nxt: 0,
                snd_wnd: 0,
                rcv_nxt: 0,
                rcv_wnd: TCP_RX_BUF_SIZE as u32,
                rx_buf: [0; TCP_RX_BUF_SIZE],
                rx_head: 0,
                rx_tail: 0,
                on_data: core::ptr::null_mut(),
                on_event: core::ptr::null_mut(),
                listen_parent: -1,
                accept_ready: 0,
                nodelay: 0,
                keepalive: 0,
            };
        }
    }
}

pub fn sync_tcp_pcbs_from_smoltcp(iface: &mut Interface, socks: &mut SocketSet<'static>) {
    // This is called from `stack_poll`/`stack_teardown` while `STACK_LOCK` is
    // held, which is the same lock every other path takes before touching
    // `tcp_sockets`/`TCP_HANDLE`; `tcp_lock` additionally keeps the same accesses
    // out of syscall context on this CPU.
    tcp_lock();
    for i in 0..TCP_MAX_SOCKETS {
        // SAFETY: `tcp_sockets` is a kernel-lifetime static array, `i` is in
        // range, and `tcp_lock` is held, so this row borrow is exclusive for the
        // iteration; the calls below only touch the smoltcp socket set.
        let row = unsafe { &mut tcp_sockets[i] };
        if row.used == 0 {
            continue;
        }
        // SAFETY: `TCP_HANDLE` is read under the same lock every writer takes, so
        // the copied handle cannot race a concurrent take.
        let Some(h) = (unsafe { TCP_HANDLE[i] }) else {
            continue;
        };
        let sock = socks.get_mut::<tcp::Socket>(h);
        let st = sock.state();
        row.state = st as u32;
        if let Some(ep) = sock.local_endpoint() {
            row.local_port = ep.port;
            let IpAddress::Ipv4(a) = ep.addr;
            row.local_ip = ipv4_u32(a);
        }
        if let Some(ep) = sock.remote_endpoint() {
            row.remote_port = ep.port;
            let IpAddress::Ipv4(a) = ep.addr;
            row.remote_ip = ipv4_u32(a);
        }
        // `accept_ready` marks a *listening* slot holding an unaccepted
        // connection.  The `listen_parent < 0` test keeps an accepted child
        // (which never clears the endpoint it was listening on) from
        // advertising itself as accept-ready again.
        row.accept_ready = 0;
        if st == tcp::State::Established && sock.listen_endpoint().port != 0 && row.listen_parent < 0 {
            row.accept_ready = 1;
        }
        // `rx_head`/`rx_tail` are the C-visible copy of "a byte is buffered".
        // It must track `can_recv()` (is there data), not `may_recv()` (may this
        // socket ever receive): the latter is true for the whole life of an open
        // connection, so it reported every socket as readable forever.
        if sock.can_recv() {
            if row.rx_head == row.rx_tail {
                row.rx_tail = row.rx_head.wrapping_add(1);
            }
        } else if row.rx_head != row.rx_tail {
            row.rx_tail = row.rx_head;
        }
        if row.nodelay != 0 {
            sock.set_nagle_enabled(false);
        } else {
            sock.set_nagle_enabled(true);
        }
        if row.keepalive != 0 {
            sock.set_keep_alive(Some(smoltcp::time::Duration::from_secs(60)));
        } else {
            sock.set_keep_alive(None);
        }
        let _ = iface;
    }
    tcp_unlock();
}

pub(crate) fn alloc_tcp_smoltcp(
    i: usize,
    socks: &mut SocketSet<'static>,
) -> Option<SocketHandle> {
    // SAFETY: `i < TCP_MAX_SOCKETS` is required by every caller (each one
    // bounds-checks its slot index first), so this buffer-array access is in
    // bounds; the buffers are kernel-lifetime statics, handed to smoltcp only
    // through this one `SocketSet`.
    let rx_buf = unsafe { &mut TCP_RX_BUFS[i][..] };
    // SAFETY: as above, for the TX buffer.
    let tx_buf = unsafe { &mut TCP_TX_BUFS[i][..] };
    let rx = tcp::SocketBuffer::new(rx_buf);
    let tx = tcp::SocketBuffer::new(tx_buf);
    let s = tcp::Socket::new(rx, tx);
    Some(socks.add(s))
}

/// Remote endpoint of slot `idx` as `(network-order IPv4, port)`, straight from
/// smoltcp.
///
/// The cached C mirror in `tcp_sockets` is only refreshed by `net_poll_task`, so
/// a connection that was accepted a moment ago still reads 0 there — which made
/// `accept()` report a peer port of 0.
pub(crate) fn tcp_socket_peer(idx: usize) -> Option<(u32, u16)> {
    with_tcp_socket(idx as i32, |s| match s.remote_endpoint() {
        Some(ep) => {
            let IpAddress::Ipv4(a) = ep.addr;
            (ipv4_u32(a), ep.port)
        }
        None => (0, 0),
    })
}

/// Local endpoint of slot `idx` as `(network-order IPv4, port)`: the connected
/// endpoint, or the listen endpoint while the socket is still a listener.  The
/// address is 0 when it was never named (bound by port only), which the caller
/// fills in from the interface config.
pub(crate) fn tcp_socket_local(idx: usize) -> Option<(u32, u16)> {
    with_tcp_socket(idx as i32, |s| {
        if let Some(ep) = s.local_endpoint() {
            let IpAddress::Ipv4(a) = ep.addr;
            return (ipv4_u32(a), ep.port);
        }
        (0, s.listen_endpoint().port)
    })
}

/// Give up slot `i` without a close handshake: unlink the smoltcp socket and
/// free the C slot.
///
/// Used to hand back a connection that was already moved into a child slot but
/// can no longer be reached through any fd (accept() ran out of Ksock rows) —
/// `tcp_close` would spend up to two seconds on the FIN first, and the point
/// here is that nobody can ever send or receive on this socket again.
pub(crate) fn tcp_free_slot(i: usize) {
    if i >= TCP_MAX_SOCKETS {
        return;
    }
    // SAFETY: `i < TCP_MAX_SOCKETS` is checked above, and this take happens under
    // `tcp_lock`, the same lock every other reader/writer of `TCP_HANDLE` takes.
    let h = {
        tcp_lock();
        // SAFETY: `TCP_HANDLE[i]` is read under `tcp_lock`, the same lock every
        // other reader/writer of the table takes, so no other context can observe
        // the handle while it is being taken.
        let h = unsafe { TCP_HANDLE[i] };
        // SAFETY: the clear happens under the same lock as the read above.
        unsafe { TCP_HANDLE[i] = None };
        tcp_unlock();
        h
    };
    if let Some(h) = h {
        let _ = stack::with_iface_sockets(|_iface, socks| {
            let _ = socks.remove(h);
        });
    }
    // SAFETY: the slot is cleared under `tcp_lock` like every other access to
    // `tcp_sockets`, and the smoltcp socket has already been unlinked above, so
    // the reset cannot race a reader of either structure.
    unsafe {
        tcp_lock();
        tcp_sockets[i] = TcpSocket {
            used: 0,
            state: TCP_CLOSED,
            local_ip: 0,
            local_port: 0,
            remote_ip: 0,
            remote_port: 0,
            snd_una: 0,
            snd_nxt: 0,
            snd_wnd: 0,
            rcv_nxt: 0,
            rcv_wnd: TCP_RX_BUF_SIZE as u32,
            rx_buf: [0; TCP_RX_BUF_SIZE],
            rx_head: 0,
            rx_tail: 0,
            on_data: core::ptr::null_mut(),
            on_event: core::ptr::null_mut(),
            listen_parent: -1,
            accept_ready: 0,
            nodelay: 0,
            keepalive: 0,
        };
        tcp_unlock();
    }
}

#[path = "tcp_api.rs"]
mod tcp_api;
pub use tcp_api::*;
#[path = "tcp_accept.rs"]
mod tcp_accept;
pub use tcp_accept::*;
