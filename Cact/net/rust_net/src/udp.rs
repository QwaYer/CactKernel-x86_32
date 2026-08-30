//! UDP sockets implemented on top of smoltcp.

use smoltcp::iface::{SocketHandle, SocketSet};
use smoltcp::socket::udp;
use smoltcp::wire::IpAddress;

use crate::stack::{self};
use crate::types::{Skb, UdpSock, UDP_SOCK_MAX};

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

pub(crate) unsafe fn reset_udp_smoltcp_state() {
    UDP_HANDLE = [None; UDP_SOCK_MAX];
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
        let _ = s.bind(port);
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
    }
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
        ensure_udp_bound(i, socks);
        let Some(h) = UDP_HANDLE[i] else {
            return -1;
        };
        let s = socks.get_mut::<udp::Socket>(h);
        let slice = core::slice::from_raw_parts(data, len as usize);
        let dst_a = core::net::Ipv4Addr::from_bits(dst_ip);
        if s.send_slice(slice, (dst_a, dst_port)).is_err() {
            return -1;
        }
        len as i32
    });
    r.unwrap_or(-1)
}

/// Legacy RX path — ingress is handled by smoltcp; free stray buffers.
#[no_mangle]
pub extern "C" fn udp_input(skb_ptr: *mut Skb) {
    if !skb_ptr.is_null() {
        crate::skb::kfree_skb(skb_ptr);
    }
}

/// Legacy TX helper — unused; kept if any C references the symbol.
#[no_mangle]
pub extern "C" fn udp_output(_skb_ptr: *mut Skb, _dst_ip: u32, _src_port: u16, _dst_port: u16) -> i32 {
    -1
}

#[no_mangle]
pub extern "C" fn udp_sock_read_ready(idx: i32) -> core::ffi::c_int {
    if idx < 0 || idx as usize >= UDP_SOCK_MAX {
        return 0;
    }
    unsafe {
        if udp_socks[idx as usize].used == 0 {
            return 0;
        }
        let Some(h) = UDP_HANDLE[idx as usize] else {
            return 0;
        };
        let r = stack::with_iface_sockets(|_iface, socks| {
            let s = socks.get_mut::<udp::Socket>(h);
            s.can_recv() as i32
        });
        r.unwrap_or(0)
    }
}
