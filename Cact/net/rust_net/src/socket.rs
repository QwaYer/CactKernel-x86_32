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
    unsafe { ffi_kernel::timer_ticks_get() }
}

/// True when a signal is waiting for the current task.
///
/// A blocking wait must return `-EINTR` rather than keep sleeping: a signal is
/// only acted on once the task returns to userspace (or the scheduler), so a
/// task parked in a socket read could not be killed with Ctrl+C at all.  The
/// signal is then delivered on the syscall-return path, as POSIX requires.
fn signal_pending() -> bool {
    unsafe { ffi_kernel::task_signal_pending_current() != 0 }
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
    unsafe {
        let ks = ksock_from_node(node);
        if ks.is_null() || (*ks).shutdown_rd != 0 {
            return -1;
        }
        let idx = (*ks).proto_idx;
        let nonblock = (*ks).nonblock != 0;
        let data = buf.cast::<u8>();
        if (*ks).kind == KS_TCP {
            // `tcp_recv` answers 0 both when nothing is buffered yet and when
            // the peer is done, so one call cannot tell a short read from EOF:
            // retry while the connection is still open and report 0 only once
            // the peer has closed its half.
            let deadline = now_ticks().saturating_add(TCP_READ_STALL_TICKS);
            loop {
                let n = tcp::tcp_recv(idx, data, size as u16);
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
                if (*ks).shutdown_rd != 0 {
                    return -1;
                }
                if now_ticks() >= deadline {
                    return -ETIMEDOUT;
                }
                ffi_kernel::sched_sleep_ticks(1);
            }
        }
        if (*ks).kind == KS_UDP {
            let deadline = now_ticks().saturating_add(UDP_READ_WAIT_TICKS);
            loop {
                let n = udp::udp_sock_recv(idx, data, size as u16, core::ptr::null_mut(), core::ptr::null_mut());
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
                ffi_kernel::sched_sleep_ticks(1);
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
    unsafe {
        let ks = ksock_from_node(node);
        if ks.is_null() || (*ks).shutdown_wr != 0 {
            return -1;
        }
        let idx = (*ks).proto_idx;
        let nonblock = (*ks).nonblock != 0;
        if (*ks).kind == KS_TCP {
            // tcp_send hands back what smoltcp actually enqueued: 0 means "TX
            // buffer or peer window full, retry after POLLOUT".  A blocking
            // socket waits for room the way POSIX write() would; a
            // non-blocking one reports -EAGAIN.  Short counts pass through.
            let deadline = now_ticks().saturating_add(TCP_WRITE_STALL_TICKS);
            loop {
                let n = tcp::tcp_send(idx, buf.cast::<u8>(), size as u16);
                if n != 0 {
                    return n;
                }
                if nonblock {
                    return -EAGAIN;
                }
                if signal_pending() {
                    return -EINTR;
                }
                if (*ks).shutdown_wr != 0 {
                    return -1;
                }
                if now_ticks() >= deadline {
                    return -ETIMEDOUT;
                }
                ffi_kernel::sched_sleep_ticks(1);
            }
        }
        if (*ks).kind == KS_UDP {
            // One datagram per write(), so the socket needs a peer — POSIX
            // would answer EDESTADDRREQ for an unconnected datagram socket.
            let (ip, port) = udp::udp_sock_peer(idx);
            if ip == 0 || port == 0 || size > u16::MAX as u32 {
                return -1;
            }
            let deadline = now_ticks().saturating_add(UDP_WRITE_STALL_TICKS);
            loop {
                let n = udp::udp_sock_send(idx, ip, port, buf.cast::<u8>(), size as u16);
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
                ffi_kernel::sched_sleep_ticks(1);
            }
        }
    }
    -1
}

extern "C" fn socket_open_op(node: *mut VfsNode) {
    if node.is_null() {
        return;
    }
    unsafe {
        node_refcount(node).fetch_add(1, Ordering::Relaxed);
    }
}

extern "C" fn socket_close_op(node: *mut VfsNode) {
    if node.is_null() {
        return;
    }
    unsafe {
        if node_refcount(node).fetch_sub(1, Ordering::AcqRel) > 1 {
            return;
        }
        let ks = ksock_from_node(node);
        if !ks.is_null() {
            if (*ks).kind == KS_TCP {
                let _ = tcp::tcp_close((*ks).proto_idx);
            } else if (*ks).kind == KS_UDP {
                udp::udp_sock_free((*ks).proto_idx);
            }
            (*ks).used = 0;
        }
        ffi_kernel::kfree(node.cast::<c_void>());
    }
}

extern "C" fn socket_poll_op(node: *mut VfsNode, events: u32) -> c_int {
    if node.is_null() {
        return VFS_POLLNVAL as c_int;
    }
    unsafe {
        let ks = ksock_from_node(node);
        if ks.is_null() {
            return VFS_POLLERR as c_int;
        }
        let idx = (*ks).proto_idx;
        let mut revents: u32 = 0;
        if (*ks).kind == KS_TCP {
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
        } else if (*ks).kind == KS_UDP {
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
    unsafe {
        for s in ksock_table.iter_mut() {
            s.used = 0;
        }
    }
}

#[no_mangle]
pub extern "C" fn ksock_from_node(node: *mut VfsNode) -> *mut Ksock {
    if node.is_null() {
        return core::ptr::null_mut();
    }
    unsafe {
        if (*node).type_ != VFS_SOCKET {
            return core::ptr::null_mut();
        }
        (*node).priv_.cast::<Ksock>()
    }
}

/// `fcntl(F_SETFL, O_NONBLOCK)` on a socket fd lands here (see `sys_fcntl`).
/// The flag lives on the socket, not on the file: a dup'd fd shares the same
/// open file description, so it shares the mode, which matches POSIX.
#[no_mangle]
pub extern "C" fn ksock_set_nonblock(node: *mut VfsNode, on: c_int) -> c_int {
    if node.is_null() {
        return -1;
    }
    unsafe {
        let ks = ksock_from_node(node);
        if ks.is_null() {
            return -1;
        }
        (*ks).nonblock = if on != 0 { 1 } else { 0 };
    }
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
    unsafe {
        let ks = ksock_from_node(node);
        if ks.is_null() {
            return -1;
        }
        let idx = (*ks).proto_idx;
        let (mut ip, port) = if (*ks).kind == KS_TCP {
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
        } else if (*ks).kind == KS_UDP {
            if idx < 0 || idx as usize >= UDP_SOCK_MAX {
                return -1;
            }
            if peer {
                // Datagram sockets keep their peer in a Rust-private table, so
                // there is no C-visible copy to read.
                udp::udp_sock_peer(idx)
            } else {
                let s = &udp::udp_socks[idx as usize];
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
        (*out).sin_family = AF_INET;
        (*out).sin_port = port.to_be();
        // The endpoint helpers hand back host-order numbers; a sockaddr_in wants
        // network order (what htonl() produces in the C paths).
        (*out).sin_addr = ip.to_be();
        (*out).sin_zero = [0; 8];
    }
    0
}

fn ksock_alloc() -> *mut Ksock {
    unsafe {
        for s in ksock_table.iter_mut() {
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

unsafe fn node_refcount(node: *mut VfsNode) -> &'static AtomicU32 {
    &*(core::ptr::addr_of!((*node).refcount) as *const AtomicU32)
}

unsafe fn make_socket_node(ks: *mut Ksock) -> *mut VfsNode {
    let node = ffi_kernel::kmalloc(core::mem::size_of::<VfsNode>()).cast::<VfsNode>();
    if node.is_null() {
        return core::ptr::null_mut();
    }
    core::ptr::write_bytes(node.cast::<u8>(), 0, core::mem::size_of::<VfsNode>());
    (*node).type_ = VFS_SOCKET;
    // Nothing references the node yet: `alloc_fd()` -> `file_alloc()` calls
    // open_vfs(), which takes the first reference, and the matching
    // close_vfs() frees the node.  Starting at 1 (as this did) left the
    // decrement in socket_close_op at 1, so close() never released the node,
    // its Ksock slot or its TCP/UDP slot — socket() failed after KSOCK_MAX
    // closes.  memfd/pipe start their nodes at 0 for the same reason.
    node_refcount(node).store(0, Ordering::Relaxed);
    (*node).ops = core::ptr::addr_of_mut!(SOCKET_OPS);
    (*node).priv_ = ks.cast::<c_void>();
    node
}

#[no_mangle]
pub extern "C" fn ksock_create(domain: c_int, type_: c_int, _protocol: c_int) -> *mut VfsNode {
    if domain as u16 != AF_INET {
        return core::ptr::null_mut();
    }
    unsafe {
        let ks = ksock_alloc();
        if ks.is_null() {
            return core::ptr::null_mut();
        }
        if type_ == 1 {
            let idx = tcp::tcp_socket();
            if idx < 0 {
                (*ks).used = 0;
                return core::ptr::null_mut();
            }
            (*ks).kind = KS_TCP;
            (*ks).proto_idx = idx;
        } else if type_ == 2 {
            let idx = udp::udp_sock_alloc();
            if idx < 0 {
                (*ks).used = 0;
                return core::ptr::null_mut();
            }
            (*ks).kind = KS_UDP;
            (*ks).proto_idx = idx;
        } else {
            (*ks).used = 0;
            return core::ptr::null_mut();
        }
        let node = make_socket_node(ks);
        if node.is_null() {
            if (*ks).kind == KS_TCP {
                let _ = tcp::tcp_close((*ks).proto_idx);
            } else {
                udp::udp_sock_free((*ks).proto_idx);
            }
            (*ks).used = 0;
        }
        node
    }
}

#[no_mangle]
pub extern "C" fn ksock_tcp_accept(listen_node: *mut VfsNode, peer_out: *mut SockAddrIn) -> *mut VfsNode {
    unsafe {
        let lks = ksock_from_node(listen_node);
        if lks.is_null() || (*lks).kind != KS_TCP {
            return core::ptr::null_mut();
        }
        let listen_idx = (*lks).proto_idx as usize;
        if listen_idx >= TCP_MAX_SOCKETS {
            return core::ptr::null_mut();
        }
        let ls = &mut tcp::tcp_sockets[listen_idx];
        if ls.used == 0 || ls.accept_ready == 0 {
            return core::ptr::null_mut();
        }
        let child_idx = (0..TCP_MAX_SOCKETS).find(|i| tcp::tcp_sockets[*i].used == 0);
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
            (*peer_out).sin_family = AF_INET;
            (*peer_out).sin_port = rport.to_be();
            // Host-order number from smoltcp -> network order for the sockaddr.
            (*peer_out).sin_addr = rip.to_be();
            (*peer_out).sin_zero = [0; 8];
        }
        // The connection is already moved into the child slot, so every
        // failure from here has to hand it back — otherwise the TCP slot (and
        // its two 4 KiB buffers) leaks with no fd able to reach it.
        let ks = ksock_alloc();
        if ks.is_null() {
            tcp::tcp_free_slot(ci);
            return core::ptr::null_mut();
        }
        (*ks).kind = KS_TCP;
        (*ks).proto_idx = ci as i32;
        let node = make_socket_node(ks);
        if node.is_null() {
            (*ks).used = 0;
            tcp::tcp_free_slot(ci);
        }
        node
    }
}

#[no_mangle]
pub extern "C" fn ksock_shutdown(node: *mut VfsNode, how: c_int) -> c_int {
    unsafe {
        let ks = ksock_from_node(node);
        if ks.is_null() {
            return -1;
        }
        if how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR {
            return -1;
        }
        if how == SHUT_RD || how == SHUT_RDWR {
            (*ks).shutdown_rd = 1;
        }
        if how == SHUT_WR || how == SHUT_RDWR {
            if (*ks).shutdown_wr == 0 {
                (*ks).shutdown_wr = 1;
                if (*ks).kind == KS_TCP {
                    let _ = tcp::tcp_shutdown_wr((*ks).proto_idx);
                }
            }
        }
    }
    0
}
