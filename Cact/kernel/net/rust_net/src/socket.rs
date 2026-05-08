use core::ffi::{c_char, c_int, c_void};

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
            return tcp::tcp_recv((*ks).proto_idx, buf.cast::<u8>(), size as u16);
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
    }
    -1
}

extern "C" fn socket_open_op(node: *mut VfsNode) {
    if node.is_null() {
        return;
    }
    unsafe { (*node).refcount = (*node).refcount.wrapping_add(1) }
}

extern "C" fn socket_close_op(node: *mut VfsNode) {
    if node.is_null() {
        return;
    }
    unsafe {
        if (*node).refcount > 1 {
            (*node).refcount -= 1;
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
        ffi_kernel::kfree_heap(node.cast::<c_void>());
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

unsafe fn make_socket_node(ks: *mut Ksock) -> *mut VfsNode {
    let node = ffi_kernel::kmalloc(core::mem::size_of::<VfsNode>()).cast::<VfsNode>();
    if node.is_null() {
        return core::ptr::null_mut();
    }
    core::ptr::write_bytes(node.cast::<u8>(), 0, core::mem::size_of::<VfsNode>());
    (*node).type_ = VFS_SOCKET;
    (*node).refcount = 1;
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
        let listen_proto = (*lks).proto_idx;
        for i in 0..TCP_MAX_SOCKETS {
            let s = &mut tcp::tcp_sockets[i];
            if s.used == 0 || s.accept_ready == 0 || s.listen_parent != listen_proto as i8 {
                continue;
            }
            s.accept_ready = 0;
            if !peer_out.is_null() {
                (*peer_out).sin_family = AF_INET;
                (*peer_out).sin_port = s.remote_port.to_be();
                (*peer_out).sin_addr = s.remote_ip;
                (*peer_out).sin_zero = [0; 8];
            }
            let ks = ksock_alloc();
            if ks.is_null() {
                return core::ptr::null_mut();
            }
            (*ks).kind = KS_TCP;
            (*ks).proto_idx = i as i32;
            return make_socket_node(ks);
        }
    }
    core::ptr::null_mut()
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
