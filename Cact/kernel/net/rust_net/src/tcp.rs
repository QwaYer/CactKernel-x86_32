use crate::config;
use crate::ip;
use crate::skb;
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

#[no_mangle]
pub extern "C" fn tcp_socket() -> i32 {
    unsafe {
        for (i, s) in tcp_sockets.iter_mut().enumerate() {
            if s.used == 0 {
                s.used = 1;
                s.state = TCP_CLOSED;
                s.rx_head = 0;
                s.rx_tail = 0;
                s.snd_nxt = 0xC0FF_EE00;
                s.rcv_wnd = TCP_RX_BUF_SIZE as u32;
                s.on_data = core::ptr::null_mut();
                s.on_event = core::ptr::null_mut();
                s.listen_parent = -1;
                s.accept_ready = 0;
                s.nodelay = 0;
                s.keepalive = 0;
                return i as i32;
            }
        }
    }
    -1
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

fn tcp_send_segment(s: &mut TcpSocket, flags: u8, data: *const u8, data_len: u16) -> i32 {
    unsafe {
        let skb_ptr = skb::skb_alloc();
        if skb_ptr.is_null() {
            return -1;
        }
        if !data.is_null() && data_len != 0 {
            let p = skb::skb_put(skb_ptr, data_len);
            for i in 0..data_len as usize {
                *p.add(i) = *data.add(i);
            }
        }
        let tcp = skb::skb_push(skb_ptr, core::mem::size_of::<TcpHeader>() as u16) as *mut TcpHeader;
        (*tcp).src_port = s.local_port.to_be();
        (*tcp).dst_port = s.remote_port.to_be();
        (*tcp).seq_num = s.snd_nxt.to_be();
        (*tcp).ack_num = if (flags & TCP_ACK) != 0 { s.rcv_nxt.to_be() } else { 0 };
        (*tcp).data_offset = 5 << 4;
        (*tcp).flags = flags;
        (*tcp).window = (s.rcv_wnd as u16).to_be();
        (*tcp).urgent_ptr = 0;
        (*tcp).checksum = 0;
        let total = core::mem::size_of::<TcpHeader>() as u16 + data_len;
        let pseudo = ip::ip_pseudo_checksum(config::ip_host().to_be(), s.remote_ip, IP_PROTO_TCP, total);
        let csum = crate::checksum::inet_checksum(tcp.cast::<u8>(), total);
        let mut sum = (!pseudo as u32).wrapping_add(csum as u32);
        while (sum >> 16) != 0 {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }
        (*tcp).checksum = !(sum as u16);
        if (flags & (TCP_SYN | TCP_FIN)) != 0 {
            s.snd_nxt = s.snd_nxt.wrapping_add(1);
        }
        s.snd_nxt = s.snd_nxt.wrapping_add(data_len as u32);
        let rc = ip::ip_output(skb_ptr, s.remote_ip, IP_PROTO_TCP);
        skb::skb_free(skb_ptr);
        rc
    }
}

#[no_mangle]
pub extern "C" fn tcp_connect(sock: i32, dst_ip: u32, dst_port: u16) -> i32 {
    if sock < 0 || sock as usize >= TCP_MAX_SOCKETS {
        return -1;
    }
    unsafe {
        let s = &mut tcp_sockets[sock as usize];
        if s.state != TCP_CLOSED {
            return -1;
        }
        s.remote_ip = dst_ip;
        s.remote_port = dst_port;
        s.local_ip = config::ip_host().to_be();
        static mut NEXT_PORT: u16 = 49152;
        s.local_port = NEXT_PORT;
        NEXT_PORT = NEXT_PORT.wrapping_add(1);
        s.state = TCP_SYN_SENT;
        tcp_send_segment(s, TCP_SYN, core::ptr::null(), 0)
    }
}

#[no_mangle]
pub extern "C" fn tcp_listen(sock: i32, local_port: u16) -> i32 {
    if sock < 0 || sock as usize >= TCP_MAX_SOCKETS {
        return -1;
    }
    unsafe {
        let s = &mut tcp_sockets[sock as usize];
        if s.state != TCP_CLOSED {
            return -1;
        }
        s.local_port = local_port;
        s.state = TCP_LISTEN;
    }
    0
}

#[no_mangle]
pub extern "C" fn tcp_send(sock: i32, data: *mut u8, len: u16) -> i32 {
    if sock < 0 || sock as usize >= TCP_MAX_SOCKETS {
        return -1;
    }
    unsafe {
        let s = &mut tcp_sockets[sock as usize];
        if s.state != TCP_ESTABLISHED {
            return -1;
        }
        tcp_send_segment(s, TCP_ACK | TCP_PSH, data, len)
    }
}

#[no_mangle]
pub extern "C" fn tcp_close(sock: i32) -> i32 {
    if sock < 0 || sock as usize >= TCP_MAX_SOCKETS {
        return -1;
    }
    unsafe {
        let s = &mut tcp_sockets[sock as usize];
        if s.state == TCP_ESTABLISHED {
            s.state = TCP_FIN_WAIT_1;
            return tcp_send_segment(s, TCP_FIN | TCP_ACK, core::ptr::null(), 0);
        }
        s.state = TCP_CLOSED;
        s.used = 0;
    }
    0
}

#[no_mangle]
pub extern "C" fn tcp_shutdown_wr(sock: i32) -> i32 {
    if sock < 0 || sock as usize >= TCP_MAX_SOCKETS {
        return -1;
    }
    unsafe {
        let s = &mut tcp_sockets[sock as usize];
        if s.used == 0 {
            return -1;
        }
        if s.state == TCP_ESTABLISHED {
            s.state = TCP_FIN_WAIT_1;
            return tcp_send_segment(s, TCP_FIN | TCP_ACK, core::ptr::null(), 0);
        }
        if s.state == TCP_CLOSE_WAIT {
            s.state = TCP_LAST_ACK;
            return tcp_send_segment(s, TCP_FIN | TCP_ACK, core::ptr::null(), 0);
        }
    }
    0
}

#[no_mangle]
pub extern "C" fn tcp_recv(sock: i32, buf: *mut u8, max_len: u16) -> i32 {
    if sock < 0 || sock as usize >= TCP_MAX_SOCKETS || buf.is_null() {
        return -1;
    }
    unsafe {
        let s = &mut tcp_sockets[sock as usize];
        if s.used == 0 || (s.state != TCP_ESTABLISHED && s.state != TCP_CLOSE_WAIT) {
            return -1;
        }
        let avail = s.rx_tail.wrapping_sub(s.rx_head);
        if avail == 0 {
            return 0;
        }
        let to_read = core::cmp::min(avail, max_len);
        for i in 0..to_read as usize {
            *buf.add(i) = s.rx_buf[(s.rx_head as usize + i) % TCP_RX_BUF_SIZE];
        }
        s.rx_head = s.rx_head.wrapping_add(to_read);
        to_read as i32
    }
}

#[no_mangle]
pub extern "C" fn tcp_input(skb_ptr: *mut Skb) {
    if skb_ptr.is_null() {
        return;
    }
    unsafe {
        if skb::skb_len(skb_ptr) < core::mem::size_of::<TcpHeader>() as u16 {
            skb::skb_free(skb_ptr);
            return;
        }
        let tcp = skb::skb_data(skb_ptr) as *mut TcpHeader;
        (*skb_ptr).tcp = tcp;
        let src_port = u16::from_be((*tcp).src_port);
        let dst_port = u16::from_be((*tcp).dst_port);
        let seq = u32::from_be((*tcp).seq_num);
        let ack = u32::from_be((*tcp).ack_num);
        let flags = (*tcp).flags;
        let hdr_len = (((*tcp).data_offset >> 4) & 0xF) as u16 * 4;
        if hdr_len < core::mem::size_of::<TcpHeader>() as u16 {
            skb::skb_free(skb_ptr);
            return;
        }
        let payload_len = if skb::skb_len(skb_ptr) > hdr_len { skb::skb_len(skb_ptr) - hdr_len } else { 0 };
        let payload = (tcp as *mut u8).add(hdr_len as usize);
        let src_ip = (*(*skb_ptr).ip).src_ip;

        let mut sock_idx: i32 = -1;
        for i in 0..TCP_MAX_SOCKETS {
            let c = &tcp_sockets[i];
            if c.used == 0 || c.state == TCP_LISTEN {
                continue;
            }
            if c.local_port == dst_port && c.remote_port == src_port && c.remote_ip == src_ip {
                sock_idx = i as i32;
                break;
            }
        }
        if sock_idx < 0 {
            for i in 0..TCP_MAX_SOCKETS {
                let c = &tcp_sockets[i];
                if c.used != 0 && c.state == TCP_LISTEN && c.local_port == dst_port {
                    sock_idx = i as i32;
                    break;
                }
            }
        }
        if sock_idx < 0 {
            skb::skb_free(skb_ptr);
            return;
        }
        let s = &mut tcp_sockets[sock_idx as usize];
        match s.state {
            TCP_LISTEN => {
                if (flags & TCP_SYN) != 0 {
                    let child_idx = tcp_socket();
                    if child_idx >= 0 {
                        let ch = &mut tcp_sockets[child_idx as usize];
                        ch.local_port = s.local_port;
                        ch.local_ip = config::ip_host().to_be();
                        ch.remote_ip = src_ip;
                        ch.remote_port = src_port;
                        ch.rcv_nxt = seq.wrapping_add(1);
                        ch.snd_una = ch.snd_nxt;
                        ch.state = TCP_SYN_RECEIVED;
                        ch.listen_parent = sock_idx as i8;
                        ch.accept_ready = 0;
                        let _ = tcp_send_segment(ch, TCP_SYN | TCP_ACK, core::ptr::null(), 0);
                    }
                }
            }
            TCP_SYN_SENT => {
                if (flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) {
                    s.rcv_nxt = seq.wrapping_add(1);
                    s.snd_una = ack;
                    s.state = TCP_ESTABLISHED;
                    let _ = tcp_send_segment(s, TCP_ACK, core::ptr::null(), 0);
                }
            }
            TCP_SYN_RECEIVED => {
                if (flags & TCP_ACK) != 0 && ack == s.snd_nxt {
                    s.snd_una = ack;
                    s.state = TCP_ESTABLISHED;
                    s.accept_ready = 1;
                }
            }
            TCP_ESTABLISHED => {
                if (flags & TCP_FIN) != 0 {
                    s.rcv_nxt = s.rcv_nxt.wrapping_add(1);
                    s.state = TCP_CLOSE_WAIT;
                    let _ = tcp_send_segment(s, TCP_ACK, core::ptr::null(), 0);
                } else if payload_len != 0 && seq == s.rcv_nxt {
                    s.rcv_nxt = s.rcv_nxt.wrapping_add(payload_len as u32);
                    for i in 0..payload_len as usize {
                        s.rx_buf[s.rx_tail as usize % TCP_RX_BUF_SIZE] = *payload.add(i);
                        s.rx_tail = s.rx_tail.wrapping_add(1);
                    }
                    let _ = tcp_send_segment(s, TCP_ACK, core::ptr::null(), 0);
                }
            }
            TCP_FIN_WAIT_1 => {
                if (flags & TCP_ACK) != 0 {
                    s.state = TCP_FIN_WAIT_2;
                }
            }
            TCP_FIN_WAIT_2 => {
                if (flags & TCP_FIN) != 0 {
                    s.rcv_nxt = s.rcv_nxt.wrapping_add(1);
                    let _ = tcp_send_segment(s, TCP_ACK, core::ptr::null(), 0);
                    s.state = TCP_CLOSED;
                    s.used = 0;
                }
            }
            _ => {}
        }
        skb::skb_free(skb_ptr);
    }
}
