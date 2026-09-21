//! `ksock_table` and VFS vtable glue for TCP/UDP sockets exposed to the rest of the kernel.
//!
//! Each open socket ties a `VfsNode` to a row in the fixed-size socket table.

use core::ffi::{c_char, c_int, c_void};
use core::sync::atomic::{AtomicU32, Ordering};

use crate::ffi_kernel;
use crate::tcp;
use crate::types::*;
use crate::udp;

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
}; KSOCK_MAX];

extern "C" fn socket_read_op(node: *mut VfsNode, _off: u32, size: u32, buf: *mut c_char) -> c_int {
    if node.is_null() {
        return -1;
    }
    unsafe {
        let ks = ksock_from_node(node);
        if ks.is_null() || (*ks).shutdown_rd != 0 {
            return -1;
        }
        if (*ks).kind == KS_TCP {
            let idx = (*ks).proto_idx;
            // `tcp_recv` answers 0 both when nothing is buffered yet and when
            // the peer is done, so one call cannot tell a short read from EOF.
            // A blocking socket read waits for the first byte instead: retry
            // while the connection is still open and report 0 only once the
            // peer has closed its half.
            loop {
                let n = tcp::tcp_recv(idx, buf.cast::<u8>(), size as u16);
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
                    _ => ffi_kernel::sched_sleep_ticks(1),
                }
            }
        }
        if (*ks).kind == KS_UDP {
            return udp::udp_sock_recv((*ks).proto_idx, buf.cast::<u8>(), size as u16, core::ptr::null_mut(), core::ptr::null_mut());
        }
    }
    -1
}

extern "C" fn socket_write_op(node: *mut VfsNode, _off: u32, size: u32, buf: *mut c_char) -> c_int {
    if node.is_null() {
        return -1;
    }
    unsafe {
        let ks = ksock_from_node(node);
        if ks.is_null() || (*ks).shutdown_wr != 0 {
            return -1;
        }
        if (*ks).kind == KS_TCP {
            return tcp::tcp_send((*ks).proto_idx, buf.cast::<u8>(), size as u16);
        }
        if (*ks).kind == KS_UDP {
            // One datagram per write(), so the socket needs a peer — POSIX
            // would answer EDESTADDRREQ for an unconnected datagram socket.
            let idx = (*ks).proto_idx;
            let (ip, port) = udp::udp_sock_peer(idx);
            if ip == 0 || port == 0 || size > u16::MAX as u32 {
                return -1;
            }
            return udp::udp_sock_send(idx, ip, port, buf.cast::<u8>(), size as u16);
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
        let mut revents: u32 = 0;
        if (*ks).kind == KS_TCP {
            let idx = (*ks).proto_idx as usize;
            if idx >= TCP_MAX_SOCKETS {
                return VFS_POLLERR as c_int;
            }
            let (used, rx_head, rx_tail) = {
                let s = &tcp::tcp_sockets[idx];
                (s.used, s.rx_head, s.rx_tail)
            };
            if used == 0 {
                return VFS_POLLERR as c_int;
            }
            if events & VFS_POLLIN != 0 && rx_head != rx_tail {
                revents |= VFS_POLLIN;
            }
            if events & VFS_POLLOUT != 0 {
                // Ask smoltcp instead of trusting the cached state: an
                // ESTABLISHED socket whose TX buffer (or the peer's window) is
                // full is not writable, and claiming otherwise makes a
                // write->0->poll retry loop spin.
                if tcp::tcp_can_send(idx) {
                    revents |= VFS_POLLOUT;
                }
            }
        } else if (*ks).kind == KS_UDP {
            if events & VFS_POLLIN != 0 {
                let idx = (*ks).proto_idx as usize;
                if idx < UDP_SOCK_MAX {
                    let s = &udp::udp_socks[idx];
                    if s.rx_ready != 0 {
                        revents |= VFS_POLLIN;
                    }
                }
            }
            if events & VFS_POLLOUT != 0 {
                // Same reasoning as TCP: an unbound or TX-buffer-full datagram
                // socket is not writable, and a dishonest POLLOUT makes a
                // write->0->poll retry loop spin.
                if udp::udp_can_send((*ks).proto_idx as usize) {
                    revents |= VFS_POLLOUT;
                }
            }
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

fn ksock_alloc() -> *mut Ksock {
    unsafe {
        for s in ksock_table.iter_mut() {
            if s.used == 0 {
                s.used = 1;
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
        let peer = &tcp::tcp_sockets[ci];
        if !peer_out.is_null() {
            (*peer_out).sin_family = AF_INET;
            (*peer_out).sin_port = peer.remote_port.to_be();
            (*peer_out).sin_addr = peer.remote_ip;
            (*peer_out).sin_zero = [0; 8];
        }
        let ks = ksock_alloc();
        if ks.is_null() {
            return core::ptr::null_mut();
        }
        (*ks).kind = KS_TCP;
        (*ks).proto_idx = ci as i32;
        make_socket_node(ks)
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
