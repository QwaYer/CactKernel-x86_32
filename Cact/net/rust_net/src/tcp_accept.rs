//! accept() hand-off between a listening slot and an established child slot.

use smoltcp::iface::SocketHandle;
use smoltcp::socket::tcp;

use crate::stack::{self};
use crate::tcp::{alloc_tcp_smoltcp, tcp_lock, tcp_sockets, tcp_unlock, TCP_HANDLE};
use crate::types::*;

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
    let (lh, lp) = {
        let r = stack::with_iface_sockets(|_iface, socks| unsafe {
            tcp_lock();
            let lh = TCP_HANDLE[li];
            tcp_unlock();
            let lh = match lh {
                Some(h) => h,
                None => return (None, 0u16),
            };
            let sock = socks.get_mut::<tcp::Socket>(lh);
            if sock.state() != tcp::State::Established {
                return (None, 0u16);
            }
            let lp = sock.listen_endpoint().port;
            if lp == 0 {
                return (None, 0u16);
            }
            (Some(lh), lp)
        });
        match r {
            Some((Some(lh), lp)) => (lh, lp),
            _ => return -1,
        }
    };
    stack::with_iface_sockets(|_iface, socks| unsafe {
        tcp_lock();
        if tcp_sockets[ci].used != 0 {
            tcp_unlock();
            return -1;
        }
        if let Some(old) = TCP_HANDLE[ci].take() {
            tcp_unlock();
            let _rm = socks.remove(old);
            core::mem::drop(_rm);
        } else {
            tcp_unlock();
        }
        tcp_lock();
        TCP_HANDLE[ci] = Some(lh);
        TCP_HANDLE[li] = None;
        tcp_sockets[ci].used = 1;
        tcp_sockets[ci].listen_parent = listen_idx as i8;
        tcp_sockets[ci].accept_ready = 0;
        tcp_unlock();
        let Some(nh) = alloc_tcp_smoltcp(li, socks) else {
            tcp_lock();
            TCP_HANDLE[li] = Some(lh);
            TCP_HANDLE[ci] = None;
            tcp_sockets[ci].used = 0;
            tcp_sockets[ci].listen_parent = -1;
            tcp_unlock();
            return -1;
        };
        tcp_lock();
        TCP_HANDLE[li] = Some(nh);
        tcp_sockets[li].used = 1;
        tcp_sockets[li].accept_ready = 0;
        tcp_unlock();
        let nls = socks.get_mut::<tcp::Socket>(nh);
        if nls.listen(lp).is_err() {
            return -1;
        }
        0
    })
    .unwrap_or(-1)
}
