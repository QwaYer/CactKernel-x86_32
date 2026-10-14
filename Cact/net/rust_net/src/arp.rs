//! Shadow ARP (IPv4 neighbor) table.
//!
//! smoltcp keeps its neighbor mappings inside `InterfaceInner` and exposes no
//! way to enumerate them, so the kernel keeps its own table here.  Entries are
//! learned passively from the ARP frames that flow through the PHY shim
//! ([`crate::stack::CactPhy`]): every ARP request or reply carries the sender's
//! protocol -> hardware mapping, which is exactly what an ARP table records and
//! what `ip neigh` reads through [`rust_net_arp_count`] / [`rust_net_arp_get`].
//!
//! Every access happens under the stack lock: the RX path (`CactPhy::receive`)
//! already runs inside `stack_poll`, which holds it, and the readers below take
//! their own [`StackGuard`].  The lock is not recursive, so `note_frame` runs
//! lockless under the caller's hold and the readers must not be reached from the
//! RX path.

use crate::config;
use crate::ffi_kernel;
use crate::stack::StackGuard;

/// Maximum number of remembered neighbors.
pub const ARP_TABLE_MAX: usize = 16;

/// An entry is dropped after this long without a fresh sighting; 60 s mirrors
/// smoltcp's own neighbor-cache lifetime.
const ENTRY_LIFETIME_MS: u32 = 60_000;
/// `timer_ticks_get()` counts 10 ms ticks, so the lifetime above is 6000 ticks.
const ENTRY_LIFETIME_TICKS: u32 = ENTRY_LIFETIME_MS / 10;

#[derive(Clone, Copy)]
struct ArpEntry {
    used: bool,
    ip_host: u32,
    mac: [u8; 6],
    /// `timer_ticks_get()` value at the last sighting (10 ms ticks).
    last_tick: u32,
}

impl ArpEntry {
    const fn empty() -> Self {
        Self {
            used: false,
            ip_host: 0,
            mac: [0; 6],
            last_tick: 0,
        }
    }
}

/// Must match `cact_arp_entry_t` in `ioctl_abi.h` byte for byte.
#[repr(C)]
pub struct CactArpEntry {
    pub ip_host: u32,
    pub mac: [u8; 6],
    pub pad: u16,
    pub age_ms: u32,
    pub flags: u32,
}

static mut ARP_TABLE: [ArpEntry; ARP_TABLE_MAX] = [ArpEntry::empty(); ARP_TABLE_MAX];

/// Record the sender of `frame` as a neighbor if it is an Ethernet/IPv4 ARP
/// request or reply.
///
/// The caller must hold the stack lock (the RX path does, because `stack_poll`
/// holds it while it calls `Interface::poll`).
pub(crate) fn note_frame(frame: &[u8]) {
    let Some((ip_host, mac)) = parse_sender(frame) else {
        return;
    };
    // A mapping for our own address is not a neighbor.
    if ip_host == config::ip_host() {
        return;
    }
    // SAFETY: only the RX path and this module's readers touch `ARP_TABLE`, and
    // every one of them runs under `STACK_LOCK` (the RX path because the caller
    // holds it, the readers because they take a `StackGuard`); the borrow is
    // consumed before returning.
    let table = unsafe { &mut *core::ptr::addr_of_mut!(ARP_TABLE) };
    // SAFETY: `timer_ticks_get` is a kernel service that reads the tick counter;
    // it takes no arguments and is callable from this context.
    let now = unsafe { ffi_kernel::timer_ticks_get() };
    record(table, now, ip_host, mac);
}

/// Pull the sender protocol/hardware address out of an Ethernet/IPv4 ARP frame,
/// or `None` when the frame is not a usable ARP request/reply.
fn parse_sender(frame: &[u8]) -> Option<(u32, [u8; 6])> {
    // Ethernet header (14) + an Ethernet/IPv4 ARP message (28).
    if frame.len() < 42 {
        return None;
    }
    // EtherType at offset 12.
    if u16::from_be_bytes([frame[12], frame[13]]) != 0x0806 {
        return None;
    }
    // Hardware type Ethernet (1), protocol IPv4 (0x0800), 6-byte hardware and
    // 4-byte protocol address lengths.
    if u16::from_be_bytes([frame[14], frame[15]]) != 1
        || u16::from_be_bytes([frame[16], frame[17]]) != 0x0800
        || frame[18] != 6
        || frame[19] != 4
    {
        return None;
    }
    // Only request (1) and reply (2) carry a usable sender mapping.
    let oper = u16::from_be_bytes([frame[20], frame[21]]);
    if oper != 1 && oper != 2 {
        return None;
    }
    let mac = [
        frame[22], frame[23], frame[24], frame[25], frame[26], frame[27],
    ];
    // Sender protocol address (network order) as a host-order `u32`.
    let ip_host = u32::from_be_bytes([frame[28], frame[29], frame[30], frame[31]]);
    // An unspecified or non-unicast sender is not a usable neighbor.
    if mac == [0; 6] || ip_host == 0 || ip_host == 0xFFFF_FFFF {
        return None;
    }
    if (ip_host >> 24) == 127 || (ip_host >> 28) == 0xE {
        return None;
    }
    Some((ip_host, mac))
}

/// Insert or refresh `ip_host -> mac`, evicting the least recently seen entry
/// when the table is full.
fn record(table: &mut [ArpEntry; ARP_TABLE_MAX], now: u32, ip_host: u32, mac: [u8; 6]) {
    let mut free: Option<usize> = None;
    let mut oldest: Option<usize> = None;
    let mut oldest_age = 0u32;
    for (i, e) in table.iter_mut().enumerate() {
        if e.used && e.ip_host == ip_host {
            e.mac = mac;
            e.last_tick = now;
            return;
        }
        if !e.used {
            if free.is_none() {
                free = Some(i);
            }
        } else {
            let age = now.wrapping_sub(e.last_tick);
            if oldest.is_none() || age > oldest_age {
                oldest = Some(i);
                oldest_age = age;
            }
        }
    }
    let idx = free.or(oldest).unwrap_or(0);
    let e = &mut table[idx];
    e.used = true;
    e.ip_host = ip_host;
    e.mac = mac;
    e.last_tick = now;
}

/// Number of live (not aged-out) neighbors.
#[no_mangle]
pub extern "C" fn rust_net_arp_count() -> i32 {
    let _guard = StackGuard::new();
    // SAFETY: `ARP_TABLE` is only touched under the stack lock, which `_guard`
    // holds; the borrow is shared and dead before the guard drops.
    let table = unsafe { &*core::ptr::addr_of!(ARP_TABLE) };
    // SAFETY: `timer_ticks_get` is a kernel service that reads the tick counter;
    // it takes no arguments and is callable from this context.
    let now = unsafe { ffi_kernel::timer_ticks_get() };
    let mut n = 0i32;
    for e in table.iter() {
        if e.used && now.wrapping_sub(e.last_tick) < ENTRY_LIFETIME_TICKS {
            n += 1;
        }
    }
    n
}

/// Fill `*out` with the `idx`-th live neighbor.  Returns 0 on success, -1 when
/// `out` is NULL or `idx` is out of range (the range is `0 .. count`).
///
/// # Safety
///
/// `out` may be null (the call then returns -1); if non-null it must point to a
/// writable, properly aligned [`CactArpEntry`] that stays live for the call.
#[no_mangle]
pub unsafe extern "C" fn rust_net_arp_get(idx: u32, out: *mut CactArpEntry) -> i32 {
    if out.is_null() {
        return -1;
    }
    let _guard = StackGuard::new();
    // SAFETY: `ARP_TABLE` is only touched under the stack lock, which `_guard`
    // holds; the borrow is shared and dead before the guard drops.
    let table = unsafe { &*core::ptr::addr_of!(ARP_TABLE) };
    // SAFETY: `timer_ticks_get` is a kernel service that reads the tick counter;
    // it takes no arguments and is callable from this context.
    let now = unsafe { ffi_kernel::timer_ticks_get() };
    let mut seen = 0u32;
    for e in table.iter() {
        let age_ticks = now.wrapping_sub(e.last_tick);
        if !e.used || age_ticks >= ENTRY_LIFETIME_TICKS {
            continue;
        }
        if seen == idx {
            // SAFETY: the caller contract (see # Safety) makes `out` writable,
            // aligned and live, and the null check above excluded null; the
            // borrow is dead before returning.
            let dst = unsafe { &mut *out };
            dst.ip_host = e.ip_host;
            dst.mac = e.mac;
            dst.pad = 0;
            dst.age_ms = age_ticks * 10;
            dst.flags = 0;
            return 0;
        }
        seen += 1;
    }
    -1
}
