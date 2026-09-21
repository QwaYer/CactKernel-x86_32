//! TCP C ABI entry points. Split out of `tcp.rs`; shared state lives there.

use core::net::Ipv4Addr;

use smoltcp::iface::SocketHandle;
use smoltcp::socket::tcp;
use smoltcp::wire::{IpAddress, IpListenEndpoint};

use crate::ffi_kernel;
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

/// How long `connect()` waits for the handshake (ticks at 100 Hz, same budget
/// as the kernel HTTP client).  smoltcp keeps retransmitting the SYN for an
/// unreachable peer, so only this deadline turns "no answer" into a failure.
const CONNECT_TIMEOUT_TICKS: u32 = 600;

const ECONNREFUSED: i32 = 111;
const ETIMEDOUT: i32 = 110;

/// `connect()` only queues the SYN and leaves the socket in SYN-SENT; the SYN
/// is dispatched by the next stack poll and ESTABLISHED needs the peer's
/// answer.  Block until that happens — otherwise the caller's very next
/// `write()` lands on a socket that cannot send yet and fails with a bare -1.
fn wait_connected(sock: i32) -> i32 {
    let deadline = unsafe { ffi_kernel::timer_ticks_get() }.saturating_add(CONNECT_TIMEOUT_TICKS);
    loop {
        match crate::tcp::with_tcp_socket(sock, |s| s.state()) {
            Some(tcp::State::Established) => return 0,
            // A RST (closed port) drops the socket straight back to CLOSED.
            Some(tcp::State::Closed) | Some(tcp::State::TimeWait) | None => {
                return -ECONNREFUSED;
            }
            _ => {}
        }
        if unsafe { ffi_kernel::timer_ticks_get() } >= deadline {
            return -ETIMEDOUT;
        }
        unsafe { ffi_kernel::sched_sleep_ticks(1) };
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
        // A local port set by CACT_SOCKCTL_BIND wins over the ephemeral one:
        // bind() has to actually bind for connect() too, otherwise a caller
        // that asked for a specific source port silently got another one.
        let local_port = unsafe {
            tcp_lock();
            let bound = tcp_sockets[sock as usize].local_port;
            let p = if bound != 0 {
                bound
            } else {
                let p = NEXT_EPHEMERAL;
                NEXT_EPHEMERAL = NEXT_EPHEMERAL.wrapping_add(1);
                if NEXT_EPHEMERAL < 49152 {
                    NEXT_EPHEMERAL = 49152;
                }
                p
            };
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
    if r.unwrap_or(-1) != 0 {
        return -1;
    }
    wait_connected(sock)
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
            // Hand back exactly what smoltcp enqueued.  It can be a short count
            // (the send window filled up mid-write) and it is 0 when nothing
            // fit at all — both mean "retry the rest after POLLOUT", not a
            // failure.  Err(SendError::InvalidState) is smoltcp's only error
            // variant and means the socket stopped being sendable.
            match s.send_slice(sl) {
                Ok(n) => n as i32,
                Err(_) => -1,
            }
        }
    });
    r.unwrap_or(-1)
}

/// How long `close()` waits for queued data and our FIN to reach the peer
/// (ticks at 100 Hz, same time base as `wait_connected`).  A live peer answers
/// within one round trip, so the deadline only bounds the wait when the peer
/// has gone silent — it is what keeps close() from holding the socket forever.
const CLOSE_TIMEOUT_TICKS: u32 = 200;

/// True once our FIN has been acknowledged (FIN-WAIT-2) or the connection is
/// gone for good.  Tearing the smoltcp socket down from here cannot discard
/// bytes we already promised the peer, which is what lets `close()` mean "the
/// peer got everything we sent" instead of "we stopped caring".
fn close_settled(state: tcp::State) -> bool {
    matches!(
        state,
        tcp::State::FinWait2 | tcp::State::TimeWait | tcp::State::Closed
    )
}

/// Queue the FIN and drive the interface until it has been acknowledged.
/// LISTEN/SYN-SENT sockets move straight to CLOSED; a connected socket keeps
/// its buffered bytes until the peer acknowledges them and the FIN.
fn wait_close_settled(h: SocketHandle) -> bool {
    if stack::with_iface_sockets(|_iface, socks| {
        socks.get_mut::<tcp::Socket>(h).close();
    })
    .is_none()
    {
        return false;
    }
    let deadline = unsafe { ffi_kernel::timer_ticks_get() }.saturating_add(CLOSE_TIMEOUT_TICKS);
    loop {
        // Poll here rather than waiting for `net_poll_task`: the timer task
        // only kicks it every NET_POLL_PERIOD_TICKS, and every close() would
        // pay that delay in full.
        stack::stack_poll();
        match stack::with_iface_sockets(|_iface, socks| socks.get_mut::<tcp::Socket>(h).state()) {
            Some(state) if close_settled(state) => return true,
            Some(_) => {}
            None => return false,
        }
        if unsafe { ffi_kernel::timer_ticks_get() } >= deadline {
            return false;
        }
        unsafe { ffi_kernel::sched_sleep_ticks(1) };
    }
}

#[no_mangle]
pub extern "C" fn tcp_close(sock: i32) -> i32 {
    if sock < 0 || sock as usize >= TCP_MAX_SOCKETS {
        return -1;
    }
    let i = sock as usize;
    let handle = unsafe {
        tcp_lock();
        let h = TCP_HANDLE[i];
        tcp_unlock();
        h
    };
    if let Some(h) = handle {
        if !wait_close_settled(h) {
            // The peer never acknowledged the FIN (or the stack is down).
            // Sending an RST says so; dropping the socket without one would
            // leave the peer waiting on a half-open connection.
            stack::with_iface_sockets(|_iface, socks| {
                socks.get_mut::<tcp::Socket>(h).abort();
            });
            stack::stack_poll();
        }
        // Unlink the handle before removing the socket: `stack_poll`'s mirror
        // update resolves the handle, and smoltcp panics on a stale one.
        unsafe {
            tcp_lock();
            TCP_HANDLE[i] = None;
            tcp_unlock();
        }
        let _ = stack::with_iface_sockets(|_iface, socks| {
            let s = socks.remove(h);
            core::mem::drop(s);
        });
    }
    unsafe {
        tcp_lock();
        tcp_sockets[i] = TcpSocket {
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
