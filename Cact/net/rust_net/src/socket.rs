//! `ksock_table` and VFS vtable glue for TCP/UDP sockets exposed to the rest of the kernel.
//!
//! Each open socket ties a `VfsNode` to a row in the fixed-size socket table.
//!
//! Readiness (`poll`) and blocking behaviour are both queried from smoltcp at
//! call time instead of trusting the cached C mirror: the mirror is refreshed
//! by `net_poll_task`, so a `select()` loop acting on it would be up to one poll
//! period stale, and a stale "readable" is what turns a selector into a spin.

use core::ffi::{c_char, c_int, c_void};
use core::sync::atomic::{AtomicU32, Ordering};

use crate::ffi_kernel;
use crate::tcp;
use crate::types::*;
use crate::udp;

/// Negative returns for read()/write() follow the kernel's `-errno`
/// convention.  Userspace's `recvfrom`/`sendto` translate them (CactLibc
/// `src/socket.c`), and Cact-dhcpd's `recvfrom(...) <= 0` retry loop treats a
/// negative as "nothing yet", so both styles of caller cope.
const EAGAIN: i32 = 11;
const ETIMEDOUT: i32 = 110;
const EINTR: i32 = 4;

/// A blocking TCP read gives up after this long without a byte while the
/// connection is still open.  Keep-alive is off by default, so smoltcp cannot
/// distinguish a peer that died silently from a slow one; without a deadline
/// the reading task would sleep here forever.
const TCP_READ_STALL_TICKS: u32 = 3000; // 30 s at 100 Hz

/// A blocking TCP write waits this long for room in the TX buffer / the peer's
/// window before giving up.
const TCP_WRITE_STALL_TICKS: u32 = 3000;

/// A blocking UDP read waits this long for a datagram, then reports 0 ("nothing
/// yet") the way this ABI always has.  Deliberately short: Cact-dhcpd polls
/// recvfrom() and sleeps between attempts, and an unbounded block would stop it
/// from ever reaching its own retransmit deadline.
const UDP_READ_WAIT_TICKS: u32 = 20; // 200 ms

/// A blocking UDP write retries while the TX buffer is transiently full.
const UDP_WRITE_STALL_TICKS: u32 = 300;

fn now_ticks() -> u32 {
    // SAFETY: `timer_ticks_get` is a kernel C service that reads the tick
    // counter; it takes no pointers and is callable from task context.
    unsafe { ffi_kernel::timer_ticks_get() }
}

/// True when a signal is waiting for the current task.
///
/// A blocking wait must return `-EINTR` rather than keep sleeping: a signal is
/// only acted on once the task returns to userspace (or the scheduler), so a
/// task parked in a socket read could not be killed with Ctrl+C at all.  The
/// signal is then delivered on the syscall-return path, as POSIX requires.
fn signal_pending() -> bool {
    // SAFETY: `task_signal_pending_current` is a kernel C service that reads the
    // current task's pending-signal word; it takes no pointers and is callable
    // from the syscall path that calls this helper.
    unsafe { sched::task::task_signal_pending_current() != 0 }
}

#[no_mangle]
pub static mut ksock_table: [Ksock; KSOCK_MAX] = [Ksock {
    used: 0,
    kind: KS_NONE,
    proto_idx: -1,
    shutdown_rd: 0,
    shutdown_wr: 0,
    so_reuseaddr: 0,
    so_keepalive: 0,
    tcp_nodelay: 0,
    so_error: 0,
    nonblock: 0,
}; KSOCK_MAX];

extern "C" fn socket_read_op(node: *mut VfsNode, _off: u32, size: u32, buf: *mut c_char) -> c_int {
    if node.is_null() {
        return -1;
    }
    if size == 0 {
        return 0;
    }
    {
        // SAFETY: the VFS layer guarantees `node` is a live `vfs_node_t` for
        // the duration of the operation, and `ksock_from_node` re-checks `type_`
        // before returning `priv_`, so `ks` is either null (checked below) or the
        // `Ksock` this node owns.
        let ks = unsafe { ksock_from_node(node) };
        if ks.is_null() {
            return -1;
        }
        let (shutdown_rd, idx, nonblock, kind) = {
            // SAFETY: `ks` is this node's live `Ksock`; the shared borrow is
            // consumed by the field copies below and is dead before the first
            // call that could recycle or mutate the row, so it cannot alias.
            let k = unsafe { &*ks };
            (k.shutdown_rd, k.proto_idx, k.nonblock != 0, k.kind)
        };
        if shutdown_rd != 0 {
            return -1;
        }
        let data = buf.cast::<u8>();
        if kind == KS_TCP {
            // `tcp_recv` answers 0 both when nothing is buffered yet and when
            // the peer is done, so one call cannot tell a short read from EOF:
            // retry while the connection is still open and report 0 only once
            // the peer has closed its half.
            let deadline = now_ticks().saturating_add(TCP_READ_STALL_TICKS);
            loop {
                // SAFETY: `idx` came from this node's `Ksock` and is a valid
                // smoltcp TCP slot index for the duration of the operation, and
                // `data` points to at least `size` writable bytes.
                let n = unsafe { tcp::tcp_recv(idx, data, size as u16) };
                if n != 0 {
                    return n;
                }
                match tcp::with_tcp_socket(idx, |s| s.state()) {
                    Some(smoltcp::socket::tcp::State::CloseWait)
                    | Some(smoltcp::socket::tcp::State::Closed)
                    | Some(smoltcp::socket::tcp::State::TimeWait)
                    | None => return 0,
                    // Reading a listener is a caller bug; fail instead of
                    // blocking on a socket that will never deliver bytes.
                    Some(smoltcp::socket::tcp::State::Listen) => return -1,
                    Some(_) => {}
                }
                if nonblock {
                    return -EAGAIN;
                }
                if signal_pending() {
                    return -EINTR;
                }
                // SAFETY: `ks` is still this node's live `Ksock` (the VFS holds
                // the fd open for the whole operation); re-read the flag after
                // the blocking wait.
                if unsafe { (*ks).shutdown_rd } != 0 {
                    return -1;
                }
                if now_ticks() >= deadline {
                    return -ETIMEDOUT;
                }
                // SAFETY: `sched_sleep_ticks` is a kernel service that suspends
                // the calling task for the requested number of ticks.
                unsafe { sched::timer_wheel::sched_sleep_ticks(1) };
            }
        }
        if kind == KS_UDP {
            let deadline = now_ticks().saturating_add(UDP_READ_WAIT_TICKS);
            loop {
                // SAFETY: `idx` is a valid smoltcp UDP slot index for the
                // operation and `data` points to at least `size` writable bytes;
                // the two null pointers select the "no peer/address output"
                // overload.
                let n = unsafe {
                    udp::udp_sock_recv(idx, data, size as u16, core::ptr::null_mut(), core::ptr::null_mut())
                };
                if n != 0 {
                    return n;
                }
                if nonblock {
                    return -EAGAIN;
                }
                if signal_pending() {
                    return -EINTR;
                }
                if now_ticks() >= deadline {
                    // 0 keeps the long-standing "nothing yet" contract for
                    // callers that never set O_NONBLOCK (dhcpd).
                    return 0;
                }
                // SAFETY: `sched_sleep_ticks` is a kernel service that suspends
                // the calling task for the requested number of ticks.
                unsafe { sched::timer_wheel::sched_sleep_ticks(1) };
            }
        }
    }
    -1
}

extern "C" fn socket_write_op(node: *mut VfsNode, _off: u32, size: u32, buf: *mut c_char) -> c_int {
    if node.is_null() {
        return -1;
    }
    if size == 0 {
        return 0;
    }
    {
        // SAFETY: the VFS layer guarantees `node` is a live `vfs_node_t` for
        // the duration of the operation, and `ksock_from_node` re-checks `type_`
        // before returning `priv_`, so `ks` is null (checked below) or this
        // node's own `Ksock`.
        let ks = unsafe { ksock_from_node(node) };
        if ks.is_null() {
            return -1;
        }
        let (shutdown_wr, idx, nonblock, kind) = {
            // SAFETY: `ks` is this node's live `Ksock`; the shared borrow is
            // consumed by the field copies below and is dead before the first
            // call that could recycle or mutate the row, so it cannot alias.
            let k = unsafe { &*ks };
            (k.shutdown_wr, k.proto_idx, k.nonblock != 0, k.kind)
        };
        if shutdown_wr != 0 {
            return -1;
        }
        if kind == KS_TCP {
            // tcp_send hands back what smoltcp actually enqueued: 0 means "TX
            // buffer or peer window full, retry after POLLOUT".  A blocking
            // socket waits for room the way POSIX write() would; a
            // non-blocking one reports -EAGAIN.  Short counts pass through.
            let deadline = now_ticks().saturating_add(TCP_WRITE_STALL_TICKS);
            loop {
                // SAFETY: `idx` came from this node's `Ksock` and is a valid
                // smoltcp TCP slot index for the operation, and `buf` points to
                // at least `size` readable bytes.
                let n = unsafe { tcp::tcp_send(idx, buf.cast::<u8>(), size as u16) };
                if n != 0 {
                    return n;
                }
                if nonblock {
                    return -EAGAIN;
                }
                if signal_pending() {
                    return -EINTR;
                }
                // SAFETY: `ks` is still this node's live `Ksock` (the VFS holds
                // the fd open for the whole operation); re-read the flag after
                // the blocking wait.
                if unsafe { (*ks).shutdown_wr } != 0 {
                    return -1;
                }
                if now_ticks() >= deadline {
                    return -ETIMEDOUT;
                }
                // SAFETY: `sched_sleep_ticks` is a kernel service that suspends
                // the calling task for the requested number of ticks.
                unsafe { sched::timer_wheel::sched_sleep_ticks(1) };
            }
        }
        if kind == KS_UDP {
            // One datagram per write(), so the socket needs a peer — POSIX
            // would answer EDESTADDRREQ for an unconnected datagram socket.
            let (ip, port) = udp::udp_sock_peer(idx);
            if ip == 0 || port == 0 || size > u16::MAX as u32 {
                return -1;
            }
            let deadline = now_ticks().saturating_add(UDP_WRITE_STALL_TICKS);
            loop {
                // SAFETY: `idx` is a valid smoltcp UDP slot index for the
                // operation and `buf` points to at least `size` readable bytes;
                // `ip`/`port` are the peer returned by `udp_sock_peer`.
                let n = unsafe { udp::udp_sock_send(idx, ip, port, buf.cast::<u8>(), size as u16) };
                if n != 0 {
                    return n;
                }
                if nonblock {
                    return -EAGAIN;
                }
                if signal_pending() {
                    return -EINTR;
                }
                if now_ticks() >= deadline {
                    return -ETIMEDOUT;
                }
                // SAFETY: `sched_sleep_ticks` is a kernel service that suspends
                // the calling task for the requested number of ticks.
                unsafe { sched::timer_wheel::sched_sleep_ticks(1) };
            }
        }
    }
    -1
}

extern "C" fn socket_open_op(node: *mut VfsNode) {
    if node.is_null() {
        return;
    }
    // SAFETY: the VFS layer guarantees `node` is a live node while `open` is
    // running, which is all `node_refcount` needs to return a reference to the
    // node's own `refcount` field; the increment is an atomic RMW.
    unsafe {
        node_refcount(node).fetch_add(1, Ordering::Relaxed);
    }
}

extern "C" fn socket_close_op(node: *mut VfsNode) {
    if node.is_null() {
        return;
    }
    // SAFETY: as in `socket_open_op`, `node` is a live node (the VFS layer holds
    // it for the duration of `close`), so `node_refcount` is sound and the
    // decrement is atomic.
    if unsafe { node_refcount(node).fetch_sub(1, Ordering::AcqRel) } > 1 {
        return;
    }
    // SAFETY: the count just reached zero, so this is the last reference to the
    // node: `ksock_from_node` therefore type-checks and returns null or the
    // exclusively-owned `Ksock` row this call is entitled to free; it re-checks
    // `type_` before `priv_` is treated as a `Ksock*`.
    let ks = unsafe { ksock_from_node(node) };
    if !ks.is_null() {
        // SAFETY: no other reference to the node or its `Ksock` row exists any
        // more, so the row may be borrowed exclusively for the rest of the call.
        let k = unsafe { &mut *ks };
        if k.kind == KS_TCP {
            let _ = tcp::tcp_close(k.proto_idx);
        } else if k.kind == KS_UDP {
            udp::udp_sock_free(k.proto_idx);
        }
        k.used = 0;
    }
    // SAFETY: the node's last reference was just released, so this call owns it
    // and may free the block `kmalloc` handed out in `make_socket_node`.
    unsafe { ffi_kernel::kfree(node.cast::<u8>()) };
}

extern "C" fn socket_poll_op(node: *mut VfsNode, events: u32) -> c_int {
    if node.is_null() {
        return VFS_POLLNVAL as c_int;
    }
    {
        // SAFETY: the VFS layer guarantees `node` is a live node during `poll`,
        // and `ksock_from_node` re-checks `type_` before handing back `priv_`,
        // so `ks` is null (checked below) or this node's own `Ksock`.
        let ks = unsafe { ksock_from_node(node) };
        if ks.is_null() {
            return VFS_POLLERR as c_int;
        }
        let (idx, kind) = {
            // SAFETY: `ks` is this node's live `Ksock`; the shared borrow is
            // consumed by the field copies below and is dead before the smoltcp
            // readiness queries, which cannot touch the row.
            let k = unsafe { &*ks };
            (k.proto_idx, k.kind)
        };
        let mut revents: u32 = 0;
        if kind == KS_TCP {
            if idx < 0 || idx as usize >= TCP_MAX_SOCKETS {
                return VFS_POLLERR as c_int;
            }
            // None = the slot has no smoltcp socket left (torn down).
            let Some((data, accept, eof)) = tcp::tcp_poll_status(idx as usize) else {
                return VFS_POLLERR as c_int;
            };
            // Readable means one of: a byte is buffered, an inbound connection
            // is waiting for accept(), or the peer closed — EOF must wake a
            // reader too, or select() on a dead connection never returns.
            if events & VFS_POLLIN != 0 && (data || accept || eof) {
                revents |= VFS_POLLIN;
            }
            if eof {
                revents |= VFS_POLLHUP;
            }
            if events & VFS_POLLOUT != 0 {
                // Ask smoltcp instead of trusting the cached state: an
                // ESTABLISHED socket whose TX buffer (or the peer's window) is
                // full is not writable, and claiming otherwise makes a
                // write->0->poll retry loop spin.
                if tcp::tcp_can_send(idx as usize) {
                    revents |= VFS_POLLOUT;
                }
            }
        } else if kind == KS_UDP {
            if idx < 0 || idx as usize >= UDP_SOCK_MAX {
                return VFS_POLLERR as c_int;
            }
            let (data, err) = udp::udp_poll_status(idx as usize);
            if err {
                return VFS_POLLERR as c_int;
            }
            if events & VFS_POLLIN != 0 && data {
                revents |= VFS_POLLIN;
            }
            if events & VFS_POLLOUT != 0 {
                // Same reasoning as TCP: an unbound or TX-buffer-full datagram
                // socket is not writable, and a dishonest POLLOUT makes a
                // write->0->poll retry loop spin.
                if udp::udp_can_send(idx as usize) {
                    revents |= VFS_POLLOUT;
                }
            }
        } else {
            return VFS_POLLERR as c_int;
        }
        revents as c_int
    }
}

static mut SOCKET_OPS: VfsOps = VfsOps {
    read: Some(socket_read_op),
    write: Some(socket_write_op),
    open: Some(socket_open_op),
    close: Some(socket_close_op),
    walk: core::ptr::null_mut(),
    readdir: core::ptr::null_mut(),
    listdir: core::ptr::null_mut(),
    create: core::ptr::null_mut(),
    delete: core::ptr::null_mut(),
    mkdir: core::ptr::null_mut(),
    rmdir: core::ptr::null_mut(),
    rename: core::ptr::null_mut(),
    symlink: core::ptr::null_mut(),
    link: core::ptr::null_mut(),
    unlink: core::ptr::null_mut(),
    readlink: core::ptr::null_mut(),
    ioctl: core::ptr::null_mut(),
    mmap_backing: core::ptr::null_mut(),
    truncate: core::ptr::null_mut(),
    chmod: core::ptr::null_mut(),
    chown: core::ptr::null_mut(),
    mknod: core::ptr::null_mut(),
    stat: core::ptr::null_mut(),
    poll: Some(socket_poll_op),
    lseek: core::ptr::null_mut(),
};

#[no_mangle]
pub extern "C" fn ksock_init() {
    // SAFETY: `ksock_table` is a kernel-lifetime static array; this runs at boot
    // before any socket exists, and the loop stays inside the array.
    unsafe {
        for s in (*core::ptr::addr_of_mut!(ksock_table)).iter_mut() {
            s.used = 0;
        }
    }
}

/// # Safety
///
/// `node` must be null or point to a live `VfsNode` (the C `vfs_node_t`) that
/// stays live for the call; when non-null its `type_` and `priv_` fields must be
/// readable, since `type_` decides whether `priv_` really is a `Ksock` pointer.
#[no_mangle]
pub unsafe extern "C" fn ksock_from_node(node: *mut VfsNode) -> *mut Ksock {
    if node.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: the caller contract (see # Safety) makes `node` a live node; the
    // shared borrow is consumed by the field reads below, which are all this
    // function does, so no aliasing can occur.
    let n = unsafe { &*node };
    if n.type_ != VFS_SOCKET {
        return core::ptr::null_mut();
    }
    // `n.priv_` is reinterpreted as `Ksock*` only after the test above proves the
    // node is a socket node, and socket nodes are the ones `make_socket_node`
    // built with `priv_` pointing at an owned `Ksock`.
    n.priv_.cast::<Ksock>()
}

/// `fcntl(F_SETFL, O_NONBLOCK)` on a socket fd lands here (see `sys_fcntl`).
/// The flag lives on the socket, not on the file: a dup'd fd shares the same
/// open file description, so it shares the mode, which matches POSIX.
///
/// # Safety
///
/// `node` must be null or a live VFS node that stays valid for the call (the VFS
/// layer holds an open fd's node); when non-null its `type_`/`priv_` fields must
/// be readable, since they decide whether it is a socket node.
#[no_mangle]
pub unsafe extern "C" fn ksock_set_nonblock(node: *mut VfsNode, on: c_int) -> c_int {
    if node.is_null() {
        return -1;
    }
    // SAFETY: `node` is non-null here and is a live VFS node, as the VFS layer
    // guarantees for a fcntl on an open fd; `ksock_from_node` re-checks the node
    // type, so `ks` is null (checked) or this node's own `Ksock`.
    let ks = unsafe { ksock_from_node(node) };
    if ks.is_null() {
        return -1;
    }
    // SAFETY: `ks` is this node's live `Ksock`, which this call owns exclusively
    // for the duration of the store.
    unsafe { (*ks).nonblock = if on != 0 { 1 } else { 0 } };
    0
}

/// Local address of a socket (`getsockname`).  Returns 0, or -1 when it has
/// none yet.
#[no_mangle]
pub extern "C" fn ksock_getsockname(node: *mut VfsNode, out: *mut SockAddrIn) -> c_int {
    sock_name(node, out, false)
}

/// Peer address of a socket (`getpeername`).  Returns 0, or -1 when the socket
/// is not connected.
#[no_mangle]
pub extern "C" fn ksock_getpeername(node: *mut VfsNode, out: *mut SockAddrIn) -> c_int {
    sock_name(node, out, true)
}

/// Shared body of the two calls above, filling a `struct sockaddr_in`
/// (`SockAddrIn`, byte-for-byte the C layout).
fn sock_name(node: *mut VfsNode, out: *mut SockAddrIn, peer: bool) -> c_int {
    if out.is_null() {
        return -1;
    }
    {
        // SAFETY: `ksock_from_node` type-checks `node`, so `ks` is null
        // (checked) or a live `Ksock`.
        let ks = unsafe { ksock_from_node(node) };
        if ks.is_null() {
            return -1;
        }
        let (idx, kind) = {
            // SAFETY: `ks` is this node's live `Ksock`; the shared borrow is
            // consumed by the field copies below and is dead before any of the
            // endpoint queries, which cannot touch the row.
            let k = unsafe { &*ks };
            (k.proto_idx, k.kind)
        };
        let (mut ip, port) = if kind == KS_TCP {
            if idx < 0 || idx as usize >= TCP_MAX_SOCKETS {
                return -1;
            }
            // Straight from smoltcp: the cached mirror is only refreshed by the
            // poll task, so it lags behind a just-connected or just-accepted
            // socket (and reported a peer of 0 for a fresh accept()).
            let ep = if peer {
                tcp::tcp_socket_peer(idx as usize)
            } else {
                tcp::tcp_socket_local(idx as usize)
            };
            match ep {
                Some(v) => v,
                None => return -1,
            }
        } else if kind == KS_UDP {
            if idx < 0 || idx as usize >= UDP_SOCK_MAX {
                return -1;
            }
            if peer {
                // Datagram sockets keep their peer in a Rust-private table, so
                // there is no C-visible copy to read.
                udp::udp_sock_peer(idx)
            } else {
                // SAFETY: `idx` is bounds-checked above, so this indexes the
                // kernel-lifetime `udp_socks` static in bounds; the borrow is
                // consumed by the two field reads on the next line.
                let s = unsafe { &udp::udp_socks[idx as usize] };
                (s.local_ip, s.local_port)
            }
        } else {
            return -1;
        };
        if peer && (ip == 0 || port == 0) {
            // Not connected — POSIX answers ENOTCONN here.
            return -1;
        }
        if port == 0 {
            // Bound to nothing yet: report the wildcard address, as POSIX does.
            ip = 0;
        } else if ip == 0 {
            // Bound to a port only: report the address the interface currently
            // holds, in host order — the `to_be()` below puts it on the wire the
            // same way the C paths' htonl() does.
            ip = crate::config::ip_host();
        }
        // SAFETY: the caller contract of `ksock_getsockname`/`ksock_getpeername`
        // makes `out` (checked non-null above) a writable `SockAddrIn` that
        // stays live for the call, so the borrow below is valid and exclusive.
        let out = unsafe { &mut *out };
        out.sin_family = AF_INET;
        out.sin_port = port.to_be();
        // The endpoint helpers hand back host-order numbers; a sockaddr_in wants
        // network order (what htonl() produces in the C paths).
        out.sin_addr = ip.to_be();
        out.sin_zero = [0; 8];
    }
    0
}

fn ksock_alloc() -> *mut Ksock {
    // SAFETY: `ksock_table` is a kernel-lifetime static array; the scan stays
    // inside it and returns a pointer to one of its rows, which the caller then
    // owns exclusively (the row is marked `used` before it is returned).
    unsafe {
        for s in (*core::ptr::addr_of_mut!(ksock_table)).iter_mut() {
            if s.used == 0 {
                // Clear the whole row: a recycled slot used to keep the
                // previous socket's shutdown flags and options (a closed
                // socket that had shutdown_wr set made the next write() on
                // that slot fail).
                *s = Ksock {
                    used: 1,
                    kind: KS_NONE,
                    proto_idx: -1,
                    shutdown_rd: 0,
                    shutdown_wr: 0,
                    so_reuseaddr: 0,
                    so_keepalive: 0,
                    tcp_nodelay: 0,
                    so_error: 0,
                    nonblock: 0,
                };
                return s as *mut Ksock;
            }
        }
    }
    core::ptr::null_mut()
}

/// # Safety
///
/// `node` must be non-null and point to a live `VfsNode` whose `refcount` field
/// is a valid, properly aligned `AtomicU32` (every node in this crate is created
/// that way) and which stays live at least as long as the returned reference is
/// used.
unsafe fn node_refcount(node: *mut VfsNode) -> &'static AtomicU32 {
    // SAFETY: the caller contract (see # Safety) makes `node` a valid pointer
    // whose `refcount` field is a live, aligned 32-bit word; `addr_of!` forms
    // that field pointer without creating an intermediate reference to `node`.
    let p = unsafe { core::ptr::addr_of!((*node).refcount) };
    // SAFETY: `AtomicU32` has the same size and alignment as the `u32` field, so
    // the reinterpreted pointer is valid for the atomic accesses callers make,
    // and the node outlives the returned reference.
    unsafe { &*(p as *const AtomicU32) }
}

/// # Safety
///
/// `ks` must be a `Ksock` row returned by `ksock_alloc` that the caller owns
/// exclusively for the duration of the call, or null (a node with a null `priv_`
/// is then created).
unsafe fn make_socket_node(ks: *mut Ksock) -> *mut VfsNode {
    let node = ffi_kernel::kmalloc(core::mem::size_of::<VfsNode>() as u32).cast::<VfsNode>();
    if node.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: `kmalloc` returned `size_of::<VfsNode>()` writable bytes and the
    // null check above guarantees `node` is valid, so this zeroes exactly the
    // freshly-allocated node.
    unsafe { core::ptr::write_bytes(node.cast::<u8>(), 0, core::mem::size_of::<VfsNode>()) };
    // SAFETY: `node` is the fresh, zeroed block above and is not published yet,
    // so this borrow is exclusive and nothing else can reach it; it ends before
    // `node_refcount` re-borrows a field of the same node.
    let n = unsafe { &mut *node };
    n.type_ = VFS_SOCKET;
    n.ops = core::ptr::addr_of_mut!(SOCKET_OPS);
    n.priv_ = ks.cast::<c_void>();
    // Nothing references the node yet: `alloc_fd()` -> `file_alloc()` calls
    // open_vfs(), which takes the first reference, and the matching
    // close_vfs() frees the node.  Starting at 1 (as this did) left the
    // decrement in socket_close_op at 1, so close() never released the node,
    // its Ksock slot or its TCP/UDP slot — socket() failed after KSOCK_MAX
    // closes.  memfd/pipe start their nodes at 0 for the same reason.
    // SAFETY: `node` is still the live, exclusively-owned block and its
    // `refcount` field is a valid `AtomicU32`, which `node_refcount` requires.
    unsafe { node_refcount(node).store(0, Ordering::Relaxed) };
    node
}

#[no_mangle]
pub extern "C" fn ksock_create(domain: c_int, type_: c_int, _protocol: c_int) -> *mut VfsNode {
    if domain as u16 != AF_INET {
        return core::ptr::null_mut();
    }
    // SAFETY: `ksock_alloc` returns null (checked below) or an exclusively owned
    // free row of the kernel-lifetime `ksock_table`, which this call owns until
    // it hands the pointer to `make_socket_node`.
    let ks = ksock_alloc();
    if ks.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: `ks` is this call's exclusively-owned fresh row; the borrow ends
    // before `make_socket_node` takes ownership of the pointer, and the calls in
    // between (`tcp_socket`/`udp_sock_alloc`) cannot reach the row.
    let k = unsafe { &mut *ks };
    if type_ == 1 {
        let idx = tcp::tcp_socket();
        if idx < 0 {
            k.used = 0;
            return core::ptr::null_mut();
        }
        k.kind = KS_TCP;
        k.proto_idx = idx;
    } else if type_ == 2 {
        let idx = udp::udp_sock_alloc();
        if idx < 0 {
            k.used = 0;
            return core::ptr::null_mut();
        }
        k.kind = KS_UDP;
        k.proto_idx = idx;
    } else {
        k.used = 0;
        return core::ptr::null_mut();
    }
    // SAFETY: `ks` is a live, exclusively-owned `Ksock` row, which is the
    // contract `make_socket_node` requires.
    let node = unsafe { make_socket_node(ks) };
    if node.is_null() {
        // SAFETY: `make_socket_node` failed without taking ownership, so the row
        // is still this call's; the borrow is re-established for the cleanup.
        let k = unsafe { &mut *ks };
        if k.kind == KS_TCP {
            let _ = tcp::tcp_close(k.proto_idx);
        } else {
            udp::udp_sock_free(k.proto_idx);
        }
        k.used = 0;
    }
    node
}

/// # Safety
///
/// `listen_node` must be null or a live VFS socket node; `peer_out` must be
/// null or point to a writable `SockAddrIn` that stays live for the call.
#[no_mangle]
pub unsafe extern "C" fn ksock_tcp_accept(listen_node: *mut VfsNode, peer_out: *mut SockAddrIn) -> *mut VfsNode {
    // SAFETY: `ksock_from_node` type-checks `listen_node`, so `lks` is null
    // (checked) or a live `Ksock`.
    let lks = unsafe { ksock_from_node(listen_node) };
    if lks.is_null() {
        return core::ptr::null_mut();
    }
    let (lks_kind, listen_idx) = {
        // SAFETY: `lks` is this node's live `Ksock`; the shared borrow is
        // consumed by the field copies on the next line and is dead before any
        // other call.
        let l = unsafe { &*lks };
        (l.kind, l.proto_idx as usize)
    };
    if lks_kind != KS_TCP || listen_idx >= TCP_MAX_SOCKETS {
        return core::ptr::null_mut();
    }
    let ready = {
        // SAFETY: `listen_idx` is bounds-checked above; the borrow is consumed
        // by the readiness test and is dead before the accept transfer below.
        let ls = unsafe { &mut tcp::tcp_sockets[listen_idx] };
        ls.used != 0 && ls.accept_ready != 0
    };
    if !ready {
        return core::ptr::null_mut();
    }
    // SAFETY: `tcp_sockets` is a kernel-lifetime static array and `i` comes from
    // a range bounded by `TCP_MAX_SOCKETS`, so the scan stays inside it.
    let child_idx = (0..TCP_MAX_SOCKETS).find(|i| unsafe { tcp::tcp_sockets[*i].used == 0 });
    let Some(ci) = child_idx else {
        return core::ptr::null_mut();
    };
    if tcp::tcp_accept_transfer(listen_idx as i32, ci as i32) != 0 {
        return core::ptr::null_mut();
    }
    // The child slot's C mirror is not filled until the poll task syncs it,
    // so ask smoltcp — otherwise accept() reported a peer port of 0.
    let (rip, rport) = tcp::tcp_socket_peer(ci).unwrap_or((0, 0));
    if !peer_out.is_null() {
        // SAFETY: the caller contract (see # Safety) makes `peer_out` a writable
        // `SockAddrIn`; the null check above excluded null.
        let out = unsafe { &mut *peer_out };
        out.sin_family = AF_INET;
        out.sin_port = rport.to_be();
        // Host-order number from smoltcp -> network order for the sockaddr.
        out.sin_addr = rip.to_be();
        out.sin_zero = [0; 8];
    }
    // The connection is already moved into the child slot, so every failure from
    // here has to hand it back — otherwise the TCP slot (and its two 4 KiB
    // buffers) leaks with no fd able to reach it.
    let ks = ksock_alloc();
    if ks.is_null() {
        tcp::tcp_free_slot(ci);
        return core::ptr::null_mut();
    }
    // SAFETY: `ks` is this call's exclusively-owned fresh row, handed to
    // `make_socket_node` below; the borrow ends before that call.
    let k = unsafe { &mut *ks };
    k.kind = KS_TCP;
    k.proto_idx = ci as i32;
    // SAFETY: the row is live and exclusively owned by this call, which is the
    // contract `make_socket_node` requires.
    let node = unsafe { make_socket_node(ks) };
    if node.is_null() {
        // SAFETY: `make_socket_node` failed without taking ownership of the row.
        unsafe { (*ks).used = 0 };
        tcp::tcp_free_slot(ci);
    }
    node
}

/// `shutdown()` on a socket fd.  Sets the C-visible shutdown flags on the
/// `Ksock` row and, for TCP, asks the stack to send our FIN.
///
/// # Safety
///
/// `node` must be null or a live VFS node that stays valid for the call; when
/// non-null its `type_`/`priv_` fields must be readable, since they decide
/// whether it is a socket node.
#[no_mangle]
pub unsafe extern "C" fn ksock_shutdown(node: *mut VfsNode, how: c_int) -> c_int {
    // SAFETY: `ksock_from_node` type-checks `node`, so `ks` is null (checked) or
    // a live `Ksock`.
    let ks = unsafe { ksock_from_node(node) };
    if ks.is_null() {
        return -1;
    }
    if how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR {
        return -1;
    }
    // SAFETY: `ks` is this node's live `Ksock`; the borrow is consumed by the
    // flag updates below, and `tcp_shutdown_wr` takes only the slot index, so it
    // cannot touch the row while the borrow is live.
    let k = unsafe { &mut *ks };
    if how == SHUT_RD || how == SHUT_RDWR {
        k.shutdown_rd = 1;
    }
    if (how == SHUT_WR || how == SHUT_RDWR) && k.shutdown_wr == 0 {
        k.shutdown_wr = 1;
        if k.kind == KS_TCP {
            let _ = tcp::tcp_shutdown_wr(k.proto_idx);
        }
    }
    0
}
