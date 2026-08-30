//! TCP C ABI entry points. Split out of `tcp.rs`; shared state lives there.

use core::net::Ipv4Addr;
use core::sync::atomic::Ordering;

use smoltcp::iface::SocketHandle;
use smoltcp::socket::tcp;
use smoltcp::wire::{IpAddress, IpListenEndpoint};

use crate::stack::{self};
use crate::tcp::{alloc_tcp_smoltcp, tcp_lock, tcp_sockets, tcp_unlock, NEXT_EPHEMERAL, TCP_HANDLE};
use crate::types::*;

#[no_mangle]
pub extern "C" fn tcp_socket() -> i32 {
    if !unsafe { stack::STACK_READY } {
        return -1;
    }
    let idx = unsafe {
        tcp_lock();
        let mut found = -1i32;
        for i in 0..TCP_MAX_SOCKETS {
            if tcp_sockets[i].used == 0 {
                tcp_sockets[i].used = 1;
                tcp_sockets[i].state = TCP_CLOSED;
                tcp_sockets[i].rx_head = 0;
                tcp_sockets[i].rx_tail = 0;
                tcp_sockets[i].listen_parent = -1;
                tcp_sockets[i].accept_ready = 0;
                tcp_sockets[i].on_data = core::ptr::null_mut();
                tcp_sockets[i].on_event = core::ptr::null_mut();
                tcp_sockets[i].nodelay = 0;
                tcp_sockets[i].keepalive = 0;
                found = i as i32;
                break;
            }
        }
        tcp_unlock();
        found
    };
    if idx < 0 {
        return -1;
    }
    let r = stack::with_iface_sockets(|_iface, socks| {
        unsafe {
            let h = match alloc_tcp_smoltcp(idx as usize, socks) {
                Some(h) => h,
                None => {
                    tcp_lock();
                    tcp_sockets[idx as usize].used = 0;
                    tcp_unlock();
                    return -1;
                }
            };
            tcp_lock();
            TCP_HANDLE[idx as usize] = Some(h);
            tcp_unlock();
            idx as i32
        }
    });
    r.unwrap_or_else(|| {
        unsafe {
            tcp_lock();
            tcp_sockets[idx as usize].used = 0;
            tcp_unlock();
        }
        -1
    })
}

#[no_mangle]
pub extern "C" fn tcp_set_callbacks(sock: i32, on_data: *mut core::ffi::c_void, on_event: *mut core::ffi::c_void) {
    if sock < 0 || sock as usize >= TCP_MAX_SOCKETS {
        return;
    }
    unsafe {
        tcp_lock();
        tcp_sockets[sock as usize].on_data = on_data;
        tcp_sockets[sock as usize].on_event = on_event;
        tcp_unlock();
    }
}

#[no_mangle]
pub extern "C" fn tcp_connect(sock: i32, dst_ip: u32, dst_port: u16) -> i32 {
    if sock < 0 || sock as usize >= TCP_MAX_SOCKETS {
        return -1;
    }
    let h = unsafe {
        tcp_lock();
        let h = TCP_HANDLE[sock as usize];
        tcp_unlock();
        h
    };
    let Some(h) = h else {
        return -1;
    };
    let r = stack::with_iface_sockets(|iface, socks| {
        let s = socks.get_mut::<tcp::Socket>(h);
        if s.is_open() {
            return -1;
        }
        let local_port = unsafe {
            tcp_lock();
            let p = NEXT_EPHEMERAL;
            NEXT_EPHEMERAL = NEXT_EPHEMERAL.wrapping_add(1);
            if NEXT_EPHEMERAL < 49152 {
                NEXT_EPHEMERAL = 49152;
            }
            tcp_unlock();
            p
        };
        let cx = iface.context();
        let dst = IpAddress::Ipv4(Ipv4Addr::from_bits(dst_ip));
        if s
            .connect(cx, (dst, dst_port), IpListenEndpoint::from(local_port))
            .is_err()
        {
            return -1;
        }
        0
    });
    r.unwrap_or(-1)
}

#[no_mangle]
pub extern "C" fn tcp_listen(sock: i32, local_port: u16) -> i32 {
    if sock < 0 || sock as usize >= TCP_MAX_SOCKETS {
        return -1;
    }
    let h = unsafe {
        tcp_lock();
        let h = TCP_HANDLE[sock as usize];
        tcp_unlock();
        h
    };
    let Some(h) = h else {
        return -1;
    };
    let r = stack::with_iface_sockets(|_iface, socks| {
        let s = socks.get_mut::<tcp::Socket>(h);
        if s.listen(local_port).is_err() {
            return -1;
        }
        0
    });
    r.unwrap_or(-1)
}

#[no_mangle]
pub extern "C" fn tcp_send(sock: i32, data: *mut u8, len: u16) -> i32 {
    if sock < 0 || sock as usize >= TCP_MAX_SOCKETS || data.is_null() {
        return -1;
    }
    let h = unsafe {
        tcp_lock();
        let h = TCP_HANDLE[sock as usize];
        tcp_unlock();
        h
    };
    let Some(h) = h else {
        return -1;
    };
    let r = stack::with_iface_sockets(|_iface, socks| {
        let s = socks.get_mut::<tcp::Socket>(h);
        if !s.may_send() {
            return -1;
        }
        unsafe {
            let sl = core::slice::from_raw_parts(data, len as usize);
            if s.send_slice(sl).is_err() {
                return -1;
            }
        }
        len as i32
    });
    r.unwrap_or(-1)
}

#[no_mangle]
pub extern "C" fn tcp_close(sock: i32) -> i32 {
    if sock < 0 || sock as usize >= TCP_MAX_SOCKETS {
        return -1;
    }
    let handle = unsafe {
        tcp_lock();
        let h = TCP_HANDLE[sock as usize].take();
        tcp_unlock();
        h
    };
    if let Some(h) = handle {
        let _ = stack::with_iface_sockets(|_iface, socks| {
            let s = socks.remove(h);
            core::mem::drop(s);
        });
    }
    unsafe {
        tcp_lock();
        tcp_sockets[sock as usize] = TcpSocket {
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
    0
}

#[no_mangle]
pub extern "C" fn tcp_shutdown_wr(sock: i32) -> i32 {
    if sock < 0 || sock as usize >= TCP_MAX_SOCKETS {
        return -1;
    }
    let h = unsafe {
        tcp_lock();
        let h = TCP_HANDLE[sock as usize];
        tcp_unlock();
        h
    };
    let Some(h) = h else {
        return -1;
    };
    let r = stack::with_iface_sockets(|_iface, socks| {
        let s = socks.get_mut::<tcp::Socket>(h);
        s.close();
        0
    });
    r.unwrap_or(-1)
}

#[no_mangle]
pub extern "C" fn tcp_recv(sock: i32, buf: *mut u8, max_len: u16) -> i32 {
    if sock < 0 || sock as usize >= TCP_MAX_SOCKETS || buf.is_null() {
        return -1;
    }
    let h = unsafe {
        tcp_lock();
        let h = TCP_HANDLE[sock as usize];
        tcp_unlock();
        h
    };
    let Some(h) = h else {
        return -1;
    };
        let r = stack::with_iface_sockets(|_iface, socks| {
        let s = socks.get_mut::<tcp::Socket>(h);
        if !s.may_recv() {
            return 0;
        }
        unsafe {
            let sl = core::slice::from_raw_parts_mut(buf, max_len as usize);
            match s.recv_slice(sl) {
                Ok(n) => n as i32,
                Err(_) => 0,
            }
        }
    });
    r.unwrap_or(-1)
}

/// C ABI stub: ingress is handled by smoltcp via [`stack::stack_enqueue_rx`].
#[no_mangle]
pub extern "C" fn tcp_input(_skb_ptr: *mut Skb) {}

/// Returns true if a TCP pcb index would read without blocking (for `select` / `poll`).
#[no_mangle]
pub extern "C" fn tcp_sock_read_ready(idx: i32) -> core::ffi::c_int {
    if idx < 0 || idx as usize >= TCP_MAX_SOCKETS {
        return 0;
    }
    let used_and_accept_ready = unsafe {
        tcp_lock();
        let r = (tcp_sockets[idx as usize].used, tcp_sockets[idx as usize].accept_ready);
        tcp_unlock();
        r
    };
    if used_and_accept_ready.0 == 0 {
        return 0;
    }
    if used_and_accept_ready.1 != 0 {
        return 1;
    }
    let h = unsafe {
        tcp_lock();
        let h = TCP_HANDLE[idx as usize];
        tcp_unlock();
        h
    };
    let Some(h) = h else {
        return 0;
    };
    let r = stack::with_iface_sockets(|_iface, socks| {
        let s = socks.get_mut::<tcp::Socket>(h);
        if s.may_recv() {
            return 1;
        }
        matches!(
            s.state(),
            tcp::State::CloseWait | tcp::State::Closed | tcp::State::TimeWait
        ) as i32
    });
    r.unwrap_or(0)
}
