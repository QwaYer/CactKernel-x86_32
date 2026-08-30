//! TCP sockets on smoltcp; keeps `tcp_sockets[]` aligned with the C ABI (state, accept, select).

use core::net::Ipv4Addr;
use core::sync::atomic::{AtomicU32, Ordering};

use smoltcp::iface::{Interface, SocketHandle, SocketSet};
use smoltcp::socket::tcp;
use smoltcp::wire::{IpAddress, IpListenEndpoint};

use crate::stack::{self};
use crate::types::*;

fn ipv4_u32(a: Ipv4Addr) -> u32 {
    let o = a.octets();
    u32::from(o[0]) << 24 | u32::from(o[1]) << 16 | u32::from(o[2]) << 8 | u32::from(o[3])
}

pub(crate) fn tcp_lock() {
    unsafe {
        let flags: u32;
        core::arch::asm!("pushfd; pop {flags}", flags = out(reg) flags, options(nomem, preserves_flags));
        core::arch::asm!("cli");
        TCP_SAVED_FLAGS.store(flags, Ordering::Relaxed);
    }
    while TCP_LOCK.compare_exchange(0, 1, Ordering::Acquire, Ordering::Relaxed).is_err() {
        unsafe { core::arch::asm!("pause"); }
    }
}

pub(crate) fn tcp_unlock() {
    let flags = TCP_SAVED_FLAGS.load(Ordering::Relaxed);
    TCP_LOCK.store(0, Ordering::Release);
    if flags & (1 << 9) != 0 {
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

pub fn sync_tcp_pcbs_from_smoltcp(iface: &mut Interface, socks: &mut SocketSet<'static>) {
    unsafe {
        tcp_lock();
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
        tcp_unlock();
    }
}

pub(crate) fn alloc_tcp_smoltcp(
    i: usize,
    socks: &mut SocketSet<'static>,
) -> Option<SocketHandle> {
    unsafe {
        let rx = tcp::SocketBuffer::new(&mut TCP_RX_BUFS[i][..]);
        let tx = tcp::SocketBuffer::new(&mut TCP_TX_BUFS[i][..]);
        let s = tcp::Socket::new(rx, tx);
        Some(socks.add(s))
    }
}

#[path = "tcp_api.rs"]
mod tcp_api;
pub use tcp_api::*;
#[path = "tcp_accept.rs"]
mod tcp_accept;
pub use tcp_accept::*;
