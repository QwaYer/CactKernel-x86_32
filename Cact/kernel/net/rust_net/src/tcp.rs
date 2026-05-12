//! TCP sockets on smoltcp; keeps `tcp_sockets[]` aligned with the C ABI (state, accept, select).

use core::net::Ipv4Addr;

use smoltcp::iface::{Interface, SocketHandle, SocketSet};
use smoltcp::socket::tcp;
use smoltcp::wire::{IpAddress, IpListenEndpoint};

use crate::stack::{self};
use crate::types::*;

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

static mut TCP_RX_BUFS: [[u8; TCP_RX_BUF_SIZE]; TCP_MAX_SOCKETS] = [[0; TCP_RX_BUF_SIZE]; TCP_MAX_SOCKETS];
static mut TCP_TX_BUFS: [[u8; TCP_RX_BUF_SIZE]; TCP_MAX_SOCKETS] = [[0; TCP_RX_BUF_SIZE]; TCP_MAX_SOCKETS];
static mut TCP_HANDLE: [Option<SocketHandle>; TCP_MAX_SOCKETS] = [None; TCP_MAX_SOCKETS];

static mut NEXT_EPHEMERAL: u16 = 49152;

pub(crate) unsafe fn reset_tcp_smoltcp_state() {
    TCP_HANDLE = [None; TCP_MAX_SOCKETS];
    NEXT_EPHEMERAL = 49152;
    for s in tcp_sockets.iter_mut() {
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

fn ipv4_u32(a: Ipv4Addr) -> u32 {
    let o = a.octets();
    u32::from(o[0]) << 24 | u32::from(o[1]) << 16 | u32::from(o[2]) << 8 | u32::from(o[3])
}

pub fn sync_tcp_pcbs_from_smoltcp(iface: &mut Interface, socks: &mut SocketSet<'static>) {
    unsafe {
        for i in 0..TCP_MAX_SOCKETS {
            if tcp_sockets[i].used == 0 {
                continue;
            }
            let Some(h) = TCP_HANDLE[i] else {
                continue;
            };
            let sock = socks.get_mut::<tcp::Socket>(h);
            let st = sock.state();
            tcp_sockets[i].state = st as u32;
            if let Some(ep) = sock.local_endpoint() {
                tcp_sockets[i].local_port = ep.port;
                let IpAddress::Ipv4(a) = ep.addr;
                tcp_sockets[i].local_ip = ipv4_u32(a);
            }
            if let Some(ep) = sock.remote_endpoint() {
                tcp_sockets[i].remote_port = ep.port;
                let IpAddress::Ipv4(a) = ep.addr;
                tcp_sockets[i].remote_ip = ipv4_u32(a);
            }
            tcp_sockets[i].accept_ready = 0;
            if st == tcp::State::Established && sock.listen_endpoint().port != 0 {
                tcp_sockets[i].accept_ready = 1;
            }
            if sock.may_recv() {
                if tcp_sockets[i].rx_head == tcp_sockets[i].rx_tail {
                    tcp_sockets[i].rx_tail = tcp_sockets[i].rx_head.wrapping_add(1);
                }
            } else if tcp_sockets[i].rx_head != tcp_sockets[i].rx_tail {
                tcp_sockets[i].rx_tail = tcp_sockets[i].rx_head;
            }
            if tcp_sockets[i].nodelay != 0 {
                sock.set_nagle_enabled(false);
            } else {
                sock.set_nagle_enabled(true);
            }
            if tcp_sockets[i].keepalive != 0 {
                sock.set_keep_alive(Some(smoltcp::time::Duration::from_secs(60)));
            } else {
                sock.set_keep_alive(None);
            }
            let _ = iface;
        }
    }
}

fn alloc_tcp_smoltcp(i: usize, socks: &mut SocketSet<'static>) -> Option<SocketHandle> {
    unsafe {
        let rx = tcp::SocketBuffer::new(&mut TCP_RX_BUFS[i][..]);
        let tx = tcp::SocketBuffer::new(&mut TCP_TX_BUFS[i][..]);
        let s = tcp::Socket::new(rx, tx);
        Some(socks.add(s))
    }
}

#[no_mangle]
pub extern "C" fn tcp_socket() -> i32 {
    if !unsafe { stack::STACK_READY } {
        return -1;
    }
    let r = stack::with_iface_sockets(|_iface, socks| {
        unsafe {
            for i in 0..TCP_MAX_SOCKETS {
                if tcp_sockets[i].used == 0 {
                    let h = match alloc_tcp_smoltcp(i, socks) {
                        Some(h) => h,
                        None => return -1,
                    };
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
                    TCP_HANDLE[i] = Some(h);
                    return i as i32;
                }
            }
            -1
        }
    });
    r.unwrap_or(-1)
}

#[no_mangle]
pub extern "C" fn tcp_set_callbacks(sock: i32, on_data: *mut core::ffi::c_void, on_event: *mut core::ffi::c_void) {
    if sock < 0 || sock as usize >= TCP_MAX_SOCKETS {
        return;
    }
    unsafe {
        tcp_sockets[sock as usize].on_data = on_data;
        tcp_sockets[sock as usize].on_event = on_event;
    }
}

#[no_mangle]
pub extern "C" fn tcp_connect(sock: i32, dst_ip: u32, dst_port: u16) -> i32 {
    if sock < 0 || sock as usize >= TCP_MAX_SOCKETS {
        return -1;
    }
    let Some(h) = (unsafe { TCP_HANDLE[sock as usize] }) else {
        return -1;
    };
    let r = stack::with_iface_sockets(|iface, socks| {
        let s = socks.get_mut::<tcp::Socket>(h);
        if s.is_open() {
            return -1;
        }
        let local_port = unsafe {
            let p = NEXT_EPHEMERAL;
            NEXT_EPHEMERAL = NEXT_EPHEMERAL.wrapping_add(1);
            if NEXT_EPHEMERAL < 49152 {
                NEXT_EPHEMERAL = 49152;
            }
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
    let Some(h) = (unsafe { TCP_HANDLE[sock as usize] }) else {
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
    let Some(h) = (unsafe { TCP_HANDLE[sock as usize] }) else {
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
    unsafe {
        if let Some(h) = TCP_HANDLE[sock as usize].take() {
            let _ = stack::with_iface_sockets(|_iface, socks| {
                let s = socks.remove(h);
                core::mem::drop(s);
            });
        }
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
    }
    0
}

#[no_mangle]
pub extern "C" fn tcp_shutdown_wr(sock: i32) -> i32 {
    if sock < 0 || sock as usize >= TCP_MAX_SOCKETS {
        return -1;
    }
    let Some(h) = (unsafe { TCP_HANDLE[sock as usize] }) else {
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
    let Some(h) = (unsafe { TCP_HANDLE[sock as usize] }) else {
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
    unsafe {
        if tcp_sockets[idx as usize].used == 0 {
            return 0;
        }
        let Some(h) = TCP_HANDLE[idx as usize] else {
            return 0;
        };
        let r = stack::with_iface_sockets(|_iface, socks| {
            let s = socks.get_mut::<tcp::Socket>(h);
            if tcp_sockets[idx as usize].accept_ready != 0 {
                return 1;
            }
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
}

/// Hand off an established inbound connection from listen slot `listen_idx` to `child_idx`,
/// then recreate the listening smoltcp socket on `listen_idx`.
pub fn tcp_accept_transfer(listen_idx: i32, child_idx: i32) -> i32 {
    if listen_idx < 0
        || child_idx < 0
        || listen_idx as usize >= TCP_MAX_SOCKETS
        || child_idx as usize >= TCP_MAX_SOCKETS
    {
        return -1;
    }
    let li = listen_idx as usize;
    let ci = child_idx as usize;
    let r = stack::with_iface_sockets(|_iface, socks| unsafe {
        let Some(lh) = TCP_HANDLE[li] else {
            return -1;
        };
        let lp = {
            let sock = socks.get_mut::<tcp::Socket>(lh);
            if sock.state() != tcp::State::Established {
                return -1;
            }
            let lp = sock.listen_endpoint().port;
            if lp == 0 {
                return -1;
            }
            lp
        };
        if tcp_sockets[ci].used != 0 {
            return -1;
        }
        if let Some(old) = TCP_HANDLE[ci].take() {
            let _rm = socks.remove(old);
            core::mem::drop(_rm);
        }
        TCP_HANDLE[ci] = Some(lh);
        TCP_HANDLE[li] = None;
        tcp_sockets[ci].used = 1;
        tcp_sockets[ci].listen_parent = listen_idx as i8;
        tcp_sockets[ci].accept_ready = 0;
        let Some(nh) = alloc_tcp_smoltcp(li, socks) else {
            return -1;
        };
        TCP_HANDLE[li] = Some(nh);
        let nls = socks.get_mut::<tcp::Socket>(nh);
        if nls.listen(lp).is_err() {
            return -1;
        }
        tcp_sockets[li].used = 1;
        tcp_sockets[li].accept_ready = 0;
        0
    });
    r.unwrap_or(-1)
}
