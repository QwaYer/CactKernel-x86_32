use crate::ip;
use crate::skb;
use crate::types::{Skb, UdpHeader, UdpSock, UDP_RX_BUF_SIZE, UDP_SOCK_MAX};

#[no_mangle]
pub static mut udp_socks: [UdpSock; UDP_SOCK_MAX] = [UdpSock {
    used: 0,
    local_port: 0,
    local_ip: 0,
    rx_buf: [0; UDP_RX_BUF_SIZE],
    rx_len: 0,
    rx_ready: 0,
    last_src_ip: 0,
    last_src_port: 0,
}; UDP_SOCK_MAX];

#[no_mangle]
pub extern "C" fn udp_sock_alloc() -> i32 {
    // SAFETY: global table mutable under cooperative kernel model.
    unsafe {
        for (i, s) in udp_socks.iter_mut().enumerate() {
            if s.used == 0 {
                s.used = 1;
                s.local_port = 0;
                s.local_ip = 0;
                s.rx_ready = 0;
                s.rx_len = 0;
                return i as i32;
            }
        }
    }
    -1
}

#[no_mangle]
pub extern "C" fn udp_sock_free(idx: i32) {
    if idx < 0 || idx as usize >= UDP_SOCK_MAX {
        return;
    }
    unsafe { udp_socks[idx as usize].used = 0; }
}

#[no_mangle]
pub extern "C" fn udp_sock_find_by_port(port: u16) -> *mut UdpSock {
    // SAFETY: return stable pointer into static array.
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
    unsafe {
        let s = &mut udp_socks[idx as usize];
        if s.used == 0 || s.rx_ready == 0 {
            return 0;
        }
        let to_copy = core::cmp::min(s.rx_len, max_len);
        for i in 0..to_copy as usize {
            *buf.add(i) = s.rx_buf[i];
        }
        if !src_ip_out.is_null() {
            *src_ip_out = s.last_src_ip;
        }
        if !src_port_out.is_null() {
            *src_port_out = s.last_src_port;
        }
        s.rx_ready = 0;
        s.rx_len = 0;
        to_copy as i32
    }
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
    unsafe {
        let s = &udp_socks[idx as usize];
        if s.used == 0 {
            return -1;
        }
        let skb_ptr = skb::skb_alloc();
        if skb_ptr.is_null() {
            return -1;
        }
        let payload = skb::skb_put(skb_ptr, len);
        for i in 0..len as usize {
            *payload.add(i) = *data.add(i);
        }
        let rc = udp_output(skb_ptr, dst_ip, s.local_port, dst_port);
        skb::skb_free(skb_ptr);
        rc
    }
}

#[no_mangle]
pub extern "C" fn udp_input(skb_ptr: *mut Skb) {
    if skb_ptr.is_null() {
        return;
    }
    unsafe {
        if skb::skb_len(skb_ptr) < core::mem::size_of::<UdpHeader>() as u16 {
            skb::skb_free(skb_ptr);
            return;
        }
        let udph = skb::skb_data(skb_ptr) as *mut UdpHeader;
        (*skb_ptr).udp = udph;
        let dst_port = u16::from_be((*udph).dst_port);
        let src_port = u16::from_be((*udph).src_port);
        let udp_len = u16::from_be((*udph).length);
        let src_ip = if !(*skb_ptr).ip.is_null() { (*(*skb_ptr).ip).src_ip } else { 0 };

        (*skb_ptr).data_offset += core::mem::size_of::<UdpHeader>() as u16;
        (*skb_ptr).total_len -= core::mem::size_of::<UdpHeader>() as u16;
        let mut payload_len = if udp_len > core::mem::size_of::<UdpHeader>() as u16 {
            udp_len - core::mem::size_of::<UdpHeader>() as u16
        } else {
            0
        };
        payload_len = core::cmp::min(payload_len, skb::skb_len(skb_ptr));

        let sock = udp_sock_find_by_port(dst_port);
        if !sock.is_null() {
            let s = &mut *sock;
            let to_store = core::cmp::min(payload_len as usize, UDP_RX_BUF_SIZE - 1);
            let payload = skb::skb_data(skb_ptr);
            for i in 0..to_store {
                s.rx_buf[i] = *payload.add(i);
            }
            s.rx_len = to_store as u16;
            s.rx_ready = 1;
            s.last_src_ip = u32::from_be(src_ip);
            s.last_src_port = src_port;
        }
        skb::skb_free(skb_ptr);
    }
}

#[no_mangle]
pub extern "C" fn udp_output(skb_ptr: *mut Skb, dst_ip: u32, src_port: u16, dst_port: u16) -> i32 {
    if skb_ptr.is_null() {
        return -1;
    }
    unsafe {
        let payload_len = skb::skb_len(skb_ptr);
        let udph = skb::skb_push(skb_ptr, core::mem::size_of::<UdpHeader>() as u16) as *mut UdpHeader;
        if udph.is_null() {
            return -1;
        }
        (*udph).src_port = src_port.to_be();
        (*udph).dst_port = dst_port.to_be();
        (*udph).length = (core::mem::size_of::<UdpHeader>() as u16 + payload_len).to_be();
        (*udph).checksum = 0;
        ip::ip_output(skb_ptr, dst_ip, crate::types::IP_PROTO_UDP)
    }
}
