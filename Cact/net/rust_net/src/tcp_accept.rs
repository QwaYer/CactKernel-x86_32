//! accept() hand-off between a listening slot and an established child slot.

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
        // SAFETY: `with_iface_sockets` holds `STACK_LOCK` for the closure and
        // `tcp_lock` serializes `TCP_HANDLE` against the poll thread; both indices were
        // bounds-checked by the caller, so the handle read is in bounds.
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
    // The closure runs under `STACK_LOCK`; `tcp_lock` serializes the
    // `TCP_HANDLE`/`tcp_sockets` updates against the poll thread, and `listen_idx`/
    // `child_idx` were bounds-checked at the top of the function.
    stack::with_iface_sockets(|_iface, socks| {
        tcp_lock();
        {
            // SAFETY: `ci` is bounds-checked and `tcp_lock` is held, so this
            // shared borrow of the child slot is valid and consumed immediately.
            let child = unsafe { &tcp_sockets[ci] };
            if child.used != 0 {
                tcp_unlock();
                return -1;
            }
        }
        // SAFETY: `TCP_HANDLE[ci]` is read under `tcp_lock`, the same lock every
        // other access takes.
        let old = unsafe { TCP_HANDLE[ci] };
        // SAFETY: as above — the clear happens under the same lock.
        unsafe { TCP_HANDLE[ci] = None };
        tcp_unlock();
        if let Some(old) = old {
            let _ = socks.remove(old);
        }
        tcp_lock();
        // SAFETY: `TCP_HANDLE` is written under `tcp_lock`, as every access is.
        unsafe { TCP_HANDLE[ci] = Some(lh) };
        // SAFETY: as above.
        unsafe { TCP_HANDLE[li] = None };
        // SAFETY: `ci` is bounds-checked and `tcp_lock` is held, so this row
        // borrow is exclusive; it ends within the block.
        unsafe {
            let child = &mut tcp_sockets[ci];
            child.used = 1;
            child.listen_parent = listen_idx as i8;
            child.accept_ready = 0;
        }
        tcp_unlock();
        let Some(nh) = alloc_tcp_smoltcp(li, socks) else {
            tcp_lock();
            // SAFETY: `TCP_HANDLE` is written under `tcp_lock`, as every access
            // is; this restores the pre-transfer state.
            unsafe { TCP_HANDLE[li] = Some(lh) };
            // SAFETY: as above.
            unsafe { TCP_HANDLE[ci] = None };
            // SAFETY: `ci` is bounds-checked and `tcp_lock` is held, so this row
            // borrow is exclusive; it ends within the block.
            unsafe {
                let child = &mut tcp_sockets[ci];
                child.used = 0;
                child.listen_parent = -1;
            }
            tcp_unlock();
            return -1;
        };
        tcp_lock();
        // SAFETY: `TCP_HANDLE` is written under `tcp_lock`, as every access is;
        // the listening slot gets the freshly-built socket.
        unsafe { TCP_HANDLE[li] = Some(nh) };
        // SAFETY: `li` is bounds-checked and `tcp_lock` is held, so this row
        // borrow is exclusive; it ends within the block.
        unsafe {
            let listen = &mut tcp_sockets[li];
            listen.used = 1;
            listen.accept_ready = 0;
        }
        tcp_unlock();
        let nls = socks.get_mut::<tcp::Socket>(nh);
        if nls.listen(lp).is_err() {
            return -1;
        }
        0
    })
    .unwrap_or(-1)
}
