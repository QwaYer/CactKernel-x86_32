//! smoltcp integration: Ethernet PHY shim, interface setup, ICMP, and the central poll loop.
//!
//! IPv4 addressing is owned by userspace: `rust_net_set_ipv4_config` (see
//! `config`) pushes whatever address/gateway/DNS the network manager decided on
//! (static config or a userspace DHCP client) into the smoltcp interface.  The
//! kernel itself does not run a DHCP client anymore.

use core::net::Ipv4Addr;
use core::sync::atomic::{AtomicPtr, Ordering};

use smoltcp::iface::{Config, Interface, SocketSet, SocketStorage};
use smoltcp::phy::{Device, DeviceCapabilities, Medium, RxToken, TxToken};
use smoltcp::socket::icmp;
use smoltcp::time::Instant;
use smoltcp::wire::{EthernetAddress, HardwareAddress, IpAddress, IpCidr};

use crate::config;
use crate::ffi_kernel;
use crate::runtime;
use crate::skb;
use crate::types::Skb;

/// Size of one frame buffer: a 1500-MTU Ethernet frame is 1514 bytes (14-byte
/// header + 1500 payload), so 1536 leaves room for VLAN tags.
const PHY_MTU: usize = 1536;

/// Frame MTU advertised to smoltcp.  smoltcp's `max_transmission_unit` is the
/// *frame* size including the Ethernet header (its `ip_mtu()` subtracts 14), so
/// the standard 1500-byte IP MTU is 1514 here.  Advertising the buffer size
/// instead made smoltcp build 1522-byte IP packets — oversized on every
/// 1500-MTU link, and a silent drop for a peer that is not QEMU/slirp.
const PHY_FRAME_MTU: usize = 1514;

/// Depth of the receive queue staged between the driver and `stack_poll`.
const RX_QUEUE_LEN: usize = 8;

pub(crate) const SOCKET_SET_SIZE: usize = 40;

static mut PHY: CactPhy = CactPhy::new();
static mut IFACE: Option<Interface> = None;
static mut SOCKET_STORAGE: [SocketStorage<'static>; SOCKET_SET_SIZE] =
    [SocketStorage::EMPTY; SOCKET_SET_SIZE];
static mut SOCKET_SET: Option<SocketSet<'static>> = None;

static mut ICMP_RX_META: [icmp::PacketMetadata; 4] = [icmp::PacketMetadata::EMPTY; 4];
static mut ICMP_RX_PAYLOAD: [u8; 512] = [0; 512];
static mut ICMP_TX_META: [icmp::PacketMetadata; 4] = [icmp::PacketMetadata::EMPTY; 4];
static mut ICMP_TX_PAYLOAD: [u8; 512] = [0; 512];
pub(crate) static mut ICMP_HANDLE: Option<smoltcp::iface::SocketHandle> = None;
static mut ICMP_IDENT_BOUND: u16 = 0xFFFF;

/// Serializes IFACE / SOCKET_SET / PHY between the background poll task and
/// syscall contexts.  With more than one CPU the poll task and a syscall can
/// otherwise run `iface.poll()` at the same time: that corrupts smoltcp's rings
/// and, because `PHY` stages a received frame in a *single* slot, one of the two
/// frames is silently overwritten and lost.
static mut STACK_LOCK: [u32; 2] = [0; 2]; // kernel irq_spinlock_t: spin + saved flags

/// Protects the RX ring (`PHY.rx*`) alone.
///
/// It exists so the frame hand-off from a NIC driver (`stack_enqueue_rx`, which
/// a driver's `poll()` reaches through `netif_rx`) never waits on `STACK_LOCK`:
/// `stack_poll` calls the driver's `poll()` *while holding `STACK_LOCK`*, so
/// taking `STACK_LOCK` again there deadlocked the CPU on the first received
/// frame — interrupts off, no timer, no Ctrl+C, dhcpd hanging after one request.
///
/// Lock order is `STACK_LOCK` -> `RX_LOCK` and never the reverse: `receive()`
/// runs under `STACK_LOCK` (via `iface.poll`) and takes `RX_LOCK` inside it,
/// while the RX path takes `RX_LOCK` alone.
static mut RX_LOCK: [u32; 2] = [0; 2]; // kernel irq_spinlock_t: spin + saved flags

pub(crate) struct StackGuard;

impl StackGuard {
    #[inline]
    pub(crate) fn new() -> Self {
        // SAFETY: `STACK_LOCK` is a kernel-lifetime `irq_spinlock_t` static;
        // `addr_of_mut!` yields its stable, properly aligned address, which is what the
        // kernel's C acquire/release pair expects.
        unsafe {
            ffi_kernel::irq_spinlock_acquire(core::ptr::addr_of_mut!(STACK_LOCK).cast());
        }
        StackGuard
    }
}

impl Drop for StackGuard {
    #[inline]
    fn drop(&mut self) {
        // SAFETY: as in `StackGuard::new` — `STACK_LOCK` is a live kernel-lifetime
        // lock, and this is the matching release for the acquire in `StackGuard::new`.
        unsafe {
            ffi_kernel::irq_spinlock_release(core::ptr::addr_of_mut!(STACK_LOCK).cast());
        }
    }
}

/// Guard for the RX ring; see [`RX_LOCK`] for why it is separate from
/// [`StackGuard`].
pub(crate) struct RxGuard;

impl RxGuard {
    #[inline]
    fn new() -> Self {
        // SAFETY: `RX_LOCK` is a kernel-lifetime `irq_spinlock_t` static;
        // `addr_of_mut!` yields its stable, properly aligned address for the kernel's
        // C acquire/release pair.
        unsafe {
            ffi_kernel::irq_spinlock_acquire(core::ptr::addr_of_mut!(RX_LOCK).cast());
        }
        RxGuard
    }
}

impl Drop for RxGuard {
    #[inline]
    fn drop(&mut self) {
        // SAFETY: as in `RxGuard::new` — `RX_LOCK` is a live kernel-lifetime lock, and
        // this is the matching release for the acquire in `RxGuard::new`.
        unsafe {
            ffi_kernel::irq_spinlock_release(core::ptr::addr_of_mut!(RX_LOCK).cast());
        }
    }
}

/// Set after `stack_init` from `register_netdev`.
pub static mut STACK_READY: bool = false;

pub(crate) fn ticks_to_instant(ticks: u32) -> Instant {
    Instant::from_millis((ticks as i64).saturating_mul(10))
}

pub(crate) fn ipv4_from_host(ip: u32) -> Ipv4Addr {
    Ipv4Addr::new(
        ((ip >> 24) & 0xff) as u8,
        ((ip >> 16) & 0xff) as u8,
        ((ip >> 8) & 0xff) as u8,
        (ip & 0xff) as u8,
    )
}

fn mask_prefix_len(mask_host: u32) -> u8 {
    mask_host.count_ones() as u8
}

pub(crate) fn sync_iface_ipv4_from_config(iface: &mut Interface) {
    let ip = ipv4_from_host(config::ip_host());
    let gw = ipv4_from_host(config::gateway_host());
    iface.update_ip_addrs(|addrs| {
        addrs.clear();
        if config::ip_host() != 0 && config::netmask_host() != 0 {
            let prefix = mask_prefix_len(config::netmask_host());
            let _ = addrs.push(IpCidr::new(IpAddress::Ipv4(ip), prefix));
        }
    });
    iface.routes_mut().remove_default_ipv4_route();
    if config::gateway_host() != 0 && !gw.is_unspecified() {
        let _ = iface.routes_mut().add_default_ipv4_route(gw);
    }
}

struct CactPhy {
    /// Frames copied out of the driver's `Skb` by `stack_enqueue_rx` and
    /// drained by `receive()`.  A single slot used to hold the frame, so a NIC
    /// that delivered two frames before `net_poll_task` ran silently lost the
    /// first one.
    rx: [[u8; PHY_MTU]; RX_QUEUE_LEN],
    rx_len: [usize; RX_QUEUE_LEN],
    rx_head: usize,
    rx_tail: usize,
    /// Frames dropped because the queue was full; never reset while running.
    rx_dropped: u32,
    tx: [u8; PHY_MTU],
}

fn active_nic_ptr() -> *mut crate::types::NetDriver {
    // SAFETY: `runtime::active_nic` is a kernel-lifetime `static mut` pointer; an
    // aligned pointer-sized load through `AtomicPtr` cannot tear, and only the
    // pointer value is inspected.
    unsafe {
        AtomicPtr::from_ptr(core::ptr::addr_of_mut!(runtime::active_nic))
            .load(Ordering::Acquire)
    }
}

impl CactPhy {
    const fn new() -> Self {
        Self {
            rx: [[0; PHY_MTU]; RX_QUEUE_LEN],
            rx_len: [0; RX_QUEUE_LEN],
            rx_head: 0,
            rx_tail: 0,
            rx_dropped: 0,
            tx: [0; PHY_MTU],
        }
    }
}

struct CactRxToken<'a> {
    slice: &'a [u8],
}

impl RxToken for CactRxToken<'_> {
    fn consume<R, F>(self, f: F) -> R
    where
        F: FnOnce(&[u8]) -> R,
    {
        f(self.slice)
    }
}

struct CactTxToken<'a> {
    buf: &'a mut [u8; PHY_MTU],
}

impl TxToken for CactTxToken<'_> {
    fn consume<R, F>(self, len: usize, f: F) -> R
    where
        F: FnOnce(&mut [u8]) -> R,
    {
        let r = f(&mut self.buf[..len]);
        let nic = active_nic_ptr();
        if !nic.is_null() {
            let skb = skb::skb_alloc();
            if !skb.is_null() {
                // SAFETY: `skb` is a fresh `Skb` this call exclusively owns and
                // `len` is bounded by `PHY_MTU` (the token's buffer length), so
                // `skb_put` writes inside its payload.
                let p = unsafe { skb::skb_put(skb, len as u16) };
                if !p.is_null() {
                    // SAFETY: `p` points to `len` writable bytes inside `skb`
                    // and `self.buf` is a local frame buffer, so the two ranges
                    // cannot overlap.
                    unsafe { core::ptr::copy_nonoverlapping(self.buf.as_ptr(), p, len) };
                    // SAFETY: `nic` is the registered driver pointer (non-null
                    // checked above) and reading its `send` field only copies the
                    // function pointer.
                    let send = unsafe { (*nic).send };
                    if let Some(send) = send {
                        // The driver's TX entry point consumes `skb` (or frees
                        // it); it is a plain `extern "C"` pointer, so the call
                        // needs no `unsafe` block of its own.
                        let _ = send(skb);
                    } else {
                        skb::kfree_skb(skb);
                    }
                } else {
                    skb::kfree_skb(skb);
                }
            }
        }
        r
    }
}

impl Device for CactPhy {
    type RxToken<'a> = CactRxToken<'a> where Self: 'a;
    type TxToken<'a> = CactTxToken<'a> where Self: 'a;

    fn receive(&mut self, _timestamp: Instant) -> Option<(Self::RxToken<'_>, Self::TxToken<'_>)> {
        let (slot, n) = {
            // Only the ring indices need the lock.  The producer never writes
            // the slot `rx_head` points at (it stops one short of it), so the
            // bytes cannot change after `rx_head` advances.
            let _rx_guard = RxGuard::new();
            if self.rx_head == self.rx_tail {
                return None;
            }
            let slot = self.rx_head;
            self.rx_head = (self.rx_head + 1) % RX_QUEUE_LEN;
            (slot, self.rx_len[slot].min(PHY_MTU))
        };
        Some((
            CactRxToken {
                slice: &self.rx[slot][..n],
            },
            CactTxToken { buf: &mut self.tx },
        ))
    }

    fn transmit(&mut self, _timestamp: Instant) -> Option<Self::TxToken<'_>> {
        Some(CactTxToken { buf: &mut self.tx })
    }

    fn capabilities(&self) -> DeviceCapabilities {
        let mut c = DeviceCapabilities::default();
        c.max_transmission_unit = PHY_FRAME_MTU;
        c.max_burst_size = Some(1);
        c.medium = Medium::Ethernet;
        c
    }
}

/// Copy one Ethernet frame from the driver's `Skb` into the RX queue and wake
/// `net_poll_task`.
///
/// Historically entered from a NIC driver's `poll()`, which `stack_poll` calls
/// while holding `STACK_LOCK` (and possibly from the driver's own IRQ or worker
/// path), so the hand-off takes only [`RX_LOCK`] — the inner lock — and never
/// `STACK_LOCK`.
///
/// # Safety
///
/// `skb` must be null or a live, initialised [`Skb`] that this call owns (a
/// frame received by a NIC driver): it is either queued or freed exactly once
/// here, so the caller must not use it afterwards.
pub unsafe fn stack_enqueue_rx(skb: *mut Skb) {
    if skb.is_null() {
        return;
    }
    // SAFETY: the caller contract (see # Safety) makes `skb` a live, initialised
    // `Skb` this call owns, so `skb_len` reads a real length.
    let len = unsafe { skb::skb_len(skb) } as usize;
    if len == 0 || len > PHY_MTU {
        skb::kfree_skb(skb);
        return;
    }
    // SAFETY: as above — `skb` is live and initialised for this call, so
    // `skb_data` returns a pointer to its payload.
    let src = unsafe { skb::skb_data(skb) };
    let queued = {
        let _rx_guard = RxGuard::new();
        // STACK_READY is read under the lock so a frame cannot slip into the
        // ring between `stack_teardown` resetting it and clearing the flag.
        // SAFETY: `STACK_READY` is a kernel-lifetime `static mut bool`; a byte
        // load reads either 0 or 1 and cannot tear.
        if !unsafe { STACK_READY } {
            skb::kfree_skb(skb);
            return;
        }
        // SAFETY: `PHY` is the kernel-lifetime PHY static whose ring is protected
        // by the `RX_LOCK` just taken, so this borrow is exclusive and is dead
        // before the wake-up below.
        let phy = unsafe { &mut *core::ptr::addr_of_mut!(PHY) };
        let next = (phy.rx_tail + 1) % RX_QUEUE_LEN;
        if next == phy.rx_head {
            // Full: the stack is not draining as fast as the NIC fills it.
            // Drop the newest frame (the older ones are already in flight)
            // and keep a counter, so the loss is at least visible.
            phy.rx_dropped = phy.rx_dropped.wrapping_add(1);
            false
        } else {
            // SAFETY: `src` points to `len` initialised bytes of `skb` and
            // `phy.rx[phy.rx_tail]` is a `PHY_MTU`-byte slot with `len <=
            // PHY_MTU`; the two are distinct objects.
            unsafe { core::ptr::copy_nonoverlapping(src, phy.rx[phy.rx_tail].as_mut_ptr(), len) };
            phy.rx_len[phy.rx_tail] = len;
            phy.rx_tail = next;
            true
        }
    };
    skb::kfree_skb(skb);
    if queued {
        // Woken outside the lock: `up` can reach the scheduler, and this path may
        // hold interrupts off.
        // SAFETY: `runtime::net_sema` is a kernel-lifetime semaphore; `up` is the
        // matching kernel C release service.
        unsafe { ffi_kernel::up(core::ptr::addr_of_mut!(runtime::net_sema)) };
    }
}

/// Frames dropped because the RX queue was full (diagnostics).
pub fn stack_rx_dropped() -> u32 {
    let _rx_guard = RxGuard::new();
    // SAFETY: `PHY` is a kernel-lifetime static whose `rx_dropped` counter is only
    // written under `RX_LOCK`, which `_rx_guard` holds here; a `u32` load cannot
    // tear.
    unsafe { PHY.rx_dropped }
}

/// # Safety
///
/// The caller must be tearing the stack down from the single-threaded
/// driver-teardown path (`unregister_netdev`) and must not be using the stack
/// concurrently: this resets every stack static (`IFACE`, `SOCKET_SET`,
/// `SOCKET_STORAGE`, `PHY`, `STACK_READY`), so a concurrent `stack_poll` or
/// syscall would observe a half-torn-down stack.
pub unsafe fn stack_teardown() {
    let _stack_guard = StackGuard::new();
    // SAFETY: the caller contract (see # Safety) excludes every other user of
    // the stack, and `STACK_LOCK` is held, so these kernel-lifetime statics can
    // be reset without racing the poll task or a syscall path.
    // SAFETY: the caller contract (see # Safety) excludes every other user of
    // the stack, so the shared `SocketSet` static may be borrowed here.
    let socks = unsafe { (*core::ptr::addr_of_mut!(SOCKET_SET)).as_mut() };
    if let Some(socks) = socks {
        // SAFETY: `socks` is the live shared `SocketSet` and this call holds the
        // stack lock, which `remove_socket`'s contract requires.
        unsafe { crate::dns_resolve::remove_socket(socks) };
    }
    // SAFETY: exclusive access to the stack statics (see above).
    unsafe { IFACE = None };
    // SAFETY: as above.
    unsafe { SOCKET_SET = None };
    // SAFETY: as above.
    unsafe { ICMP_HANDLE = None };
    // SAFETY: as above.
    unsafe { ICMP_IDENT_BOUND = 0xFFFF };
    // SAFETY: `SOCKET_STORAGE` is a kernel-lifetime static array with exclusive
    // access here (see above); the borrow is dead before the resets below.
    let storage = unsafe { &mut *core::ptr::addr_of_mut!(SOCKET_STORAGE) };
    for s in storage.iter_mut() {
        *s = SocketStorage::EMPTY;
    }
    {
        // Reset the ring and STACK_READY together under the ring lock: a frame
        // arriving concurrently is either already queued (and goes away with the
        // reset) or sees the cleared flag and is freed.
        let _rx_guard = RxGuard::new();
        // SAFETY: the RX lock is held and no other stack user exists (see
        // # Safety), so resetting the PHY ring and the readiness flag cannot
        // race the RX producer.
        // SAFETY: the RX lock is held and no other stack user exists (see
        // # Safety), so resetting the PHY object cannot race the RX producer.
        unsafe { PHY = CactPhy::new() };
        // SAFETY: as above; `STACK_READY` is cleared under the RX lock so a
        // concurrent frame either is already queued or is freed.
        unsafe { STACK_READY = false };
    }
    // SAFETY: still under `STACK_LOCK` with the stack torn down, so the TCP and
    // UDP reset helpers have exclusive access to their tables, as their own
    // contracts require.
    // SAFETY: still under `STACK_LOCK` with the stack torn down, so the TCP
    // reset helper has exclusive access to its tables, as its own contract
    // requires.
    unsafe { crate::tcp::reset_tcp_smoltcp_state() };
    // SAFETY: as above, for the UDP tables.
    unsafe { crate::udp::reset_udp_smoltcp_state() };
}

pub fn stack_init() {
    let _stack_guard = StackGuard::new();
    // `stack_init` runs under `STACK_LOCK` from the driver-registration path;
    // the interface, socket set and ICMP packet-buffer statics are kernel-lifetime
    // objects and this is their only initialiser, so no other context can observe
    // them half-built.
    {
        // SAFETY: `STACK_READY` is a kernel-lifetime `static mut bool` read under
        // `STACK_LOCK`; a byte load reads either 0 or 1.
        if unsafe { STACK_READY } {
            return;
        }
        // SAFETY: `runtime::my_mac` is a kernel-lifetime `static mut` read on the
        // single-threaded registration path; a 6-byte copy cannot tear.
        let mac = unsafe { runtime::my_mac.b };
        let eth = EthernetAddress::from_bytes(&mac);
        let mut cfg = Config::new(HardwareAddress::Ethernet(eth));
        // SAFETY: `timer_ticks_get` is a kernel C service that reads the tick
        // counter; it takes no pointers and is callable from this path.
        cfg.random_seed = u64::from(unsafe { ffi_kernel::timer_ticks_get() });
        // SAFETY: as above — the same monotonic tick read.
        let now = ticks_to_instant(unsafe { ffi_kernel::timer_ticks_get() });
        // SAFETY: `PHY` is the kernel-lifetime PHY static; this borrow is consumed
        // by `Interface::new` (which does not retain the device) before the store
        // to a different static below.
        let phy = unsafe { &mut *core::ptr::addr_of_mut!(PHY) };
        let new_iface = Interface::new(cfg, phy, now);
        // SAFETY: `IFACE` is a kernel-lifetime `static mut`; this is its only
        // initialiser, under the stack lock.
        unsafe { IFACE = Some(new_iface) };
        // SAFETY: `SOCKET_STORAGE` is the kernel-lifetime backing array of the one
        // `SocketSet`; the borrow ends before the `SOCKET_SET` store below.
        let storage = unsafe { &mut *core::ptr::addr_of_mut!(SOCKET_STORAGE) };
        let new_set = SocketSet::new(&mut storage[..]);
        // SAFETY: as for `IFACE` — the only initialiser of `SOCKET_SET`, under the
        // stack lock.
        unsafe { SOCKET_SET = Some(new_set) };

        // SAFETY: `IFACE` was just set above and is only used under this lock, so
        // the borrow is exclusive for the rest of the function.
        let iface = unsafe { (*core::ptr::addr_of_mut!(IFACE)).as_mut().unwrap() };
        sync_iface_ipv4_from_config(iface);

        // SAFETY: `SOCKET_SET` was just set above and is only used under this
        // lock, so the borrow is exclusive for the rest of the function.
        let socks = unsafe { (*core::ptr::addr_of_mut!(SOCKET_SET)).as_mut().unwrap() };
        // SAFETY: `ICMP_RX_META` is a kernel-lifetime packet-metadata static handed
        // to smoltcp only through this one socket, under the stack lock.
        let rx_meta = unsafe { &mut ICMP_RX_META[..] };
        // SAFETY: as above, for `ICMP_RX_PAYLOAD`.
        let rx_payload = unsafe { &mut ICMP_RX_PAYLOAD[..] };
        let icmp_rx = icmp::PacketBuffer::new(rx_meta, rx_payload);
        // SAFETY: as above, for `ICMP_TX_META`.
        let tx_meta = unsafe { &mut ICMP_TX_META[..] };
        // SAFETY: as above, for `ICMP_TX_PAYLOAD`.
        let tx_payload = unsafe { &mut ICMP_TX_PAYLOAD[..] };
        let icmp_tx = icmp::PacketBuffer::new(tx_meta, tx_payload);
        let icmp_sock = icmp::Socket::new(icmp_rx, icmp_tx);
        // SAFETY: `ICMP_HANDLE` is a kernel-lifetime `static mut` written only
        // under the stack lock, and the handle refers to the shared `socks` set
        // kept alive above.
        unsafe { ICMP_HANDLE = Some(socks.add(icmp_sock)) };

        // SAFETY: `init_socket`'s contract needs the caller to hold the stack lock
        // (or be `stack_init`, before the stack is published), which is exactly
        // this call.
        unsafe { crate::dns_resolve::init_socket(socks) };

        // SAFETY: `STACK_READY` is published last, under the stack lock, once
        // every static above is fully built.
        unsafe { STACK_READY = true };
        ffi_kernel::klog_static(
            ffi_kernel::LOG_OK,
            b"smoltcp interface and sockets ready (poll from net_poll_task)\0",
        );
    }
}

pub fn stack_poll() {
    let _stack_guard = StackGuard::new();
    let nic = active_nic_ptr();
    // SAFETY: `nic` is the registered driver pointer (null-checked) and reading
    // its `poll` field only copies the function pointer.
    let poll = (!nic.is_null()).then(|| unsafe { (*nic).poll }).flatten();
    if let Some(poll) = poll {
        // `poll` is the driver's RX entry point; it is called here while
        // `STACK_LOCK` is held, as the driver contract requires.
        poll();
    }
    // SAFETY: `STACK_READY` is a kernel-lifetime `static mut bool`; a byte load
    // reads either 0 or 1.
    if !unsafe { STACK_READY } {
        return;
    }
    // SAFETY: `timer_ticks_get` is a kernel C service that reads the tick counter;
    // it is callable from task context.
    let now = ticks_to_instant(unsafe { ffi_kernel::timer_ticks_get() });
    // SAFETY: `stack_poll` holds `STACK_LOCK`; `IFACE`/`SOCKET_SET` are only used
    // under this lock, so these borrows are exclusive for the whole poll.
    let iface = unsafe { (*core::ptr::addr_of_mut!(IFACE)).as_mut() };
    // SAFETY: as above, for `SOCKET_SET`.
    let socks = unsafe { (*core::ptr::addr_of_mut!(SOCKET_SET)).as_mut() };
    if let (Some(iface), Some(socks)) = (iface, socks) {
        // SAFETY: `PHY` is borrowed exclusively here under the stack lock and
        // handed to the interface for the duration of the poll only.
        unsafe { iface.poll(now, &mut *core::ptr::addr_of_mut!(PHY), socks) };
        crate::tcp::sync_tcp_pcbs_from_smoltcp(iface, socks);
        crate::udp::sync_udp_pcbs_from_smoltcp(socks);
    }
}

/// Fire-and-forget ICMPv4 echo request (kernel ping helper).
pub fn icmp_echo_request_host(dst_ip_host: u32, id: u16, seq: u16) -> bool {
    let _stack_guard = StackGuard::new();
    // The ICMP statics (`ICMP_HANDLE`, `ICMP_IDENT_BOUND`, the packet buffers)
    // are accessed only under `STACK_LOCK`, and the socket handle travels with
    // the shared `SocketSet`.
    {
        // SAFETY: `STACK_READY` is a kernel-lifetime `static mut bool` read under
        // `STACK_LOCK`; a byte load reads either 0 or 1.
        if !unsafe { STACK_READY } {
            return false;
        }
        // SAFETY: `ICMP_HANDLE` is a kernel-lifetime `static mut` only accessed
        // under `STACK_LOCK`; copying the `Option` cannot tear.
        let Some(icmp_h) = (unsafe { ICMP_HANDLE }) else {
            return false;
        };
        // Only the presence of a live interface matters here; the packet is
        // emitted into the socket buffer and sent by the next poll.
        // SAFETY: holds `STACK_LOCK`; `IFACE` is only used under it, so this
        // borrow is exclusive for the check below.
        if (unsafe { (*core::ptr::addr_of_mut!(IFACE)).as_mut() }).is_none() {
            return false;
        }
        // SAFETY: as above, for `SOCKET_SET`; the borrow stays live for the rest
        // of the call, which owns it under the stack lock.
        let Some(socks) = (unsafe { (*core::ptr::addr_of_mut!(SOCKET_SET)).as_mut() }) else {
            return false;
        };
        // SAFETY: `ICMP_IDENT_BOUND` is a kernel-lifetime `static mut` read under
        // the stack lock.
        let ident_bound = unsafe { ICMP_IDENT_BOUND };
        let need_replace = {
            let s = socks.get_mut::<icmp::Socket>(icmp_h);
            s.is_open() && ident_bound != id
        };
        if need_replace {
            let _ = socks.remove(icmp_h);
            // SAFETY: `ICMP_RX_META` is a kernel-lifetime packet-metadata static
            // handed to smoltcp only through this one socket, under the stack lock.
            let rx_meta = unsafe { &mut ICMP_RX_META[..] };
            // SAFETY: as above, for `ICMP_RX_PAYLOAD`.
            let rx_payload = unsafe { &mut ICMP_RX_PAYLOAD[..] };
            let icmp_rx = icmp::PacketBuffer::new(rx_meta, rx_payload);
            // SAFETY: as above, for `ICMP_TX_META`.
            let tx_meta = unsafe { &mut ICMP_TX_META[..] };
            // SAFETY: as above, for `ICMP_TX_PAYLOAD`.
            let tx_payload = unsafe { &mut ICMP_TX_PAYLOAD[..] };
            let icmp_tx = icmp::PacketBuffer::new(tx_meta, tx_payload);
            let icmp_sock = icmp::Socket::new(icmp_rx, icmp_tx);
            // SAFETY: `ICMP_HANDLE` is written under the stack lock, as every
            // other access is.
            unsafe { ICMP_HANDLE = Some(socks.add(icmp_sock)) };
            // SAFETY: as above, for `ICMP_IDENT_BOUND`.
            unsafe { ICMP_IDENT_BOUND = 0xFFFF };
        }
        // SAFETY: `ICMP_HANDLE` is read under the stack lock and was just ensured
        // to be `Some`.
        let icmp_h = (unsafe { ICMP_HANDLE }).unwrap();
        let sock = socks.get_mut::<icmp::Socket>(icmp_h);
        if !sock.is_open() {
            if sock.bind(icmp::Endpoint::Ident(id)).is_err() {
                return false;
            }
            // SAFETY: `ICMP_IDENT_BOUND` is written under the stack lock.
            unsafe { ICMP_IDENT_BOUND = id };
        }
        let dst = IpAddress::Ipv4(ipv4_from_host(dst_ip_host));
        use smoltcp::phy::ChecksumCapabilities;
        use smoltcp::wire::{Icmpv4Packet, Icmpv4Repr};
        /* 56 bytes of data make the ICMP message 64 bytes, matching the default
         * Linux `ping` payload so the reply sizes read the same. */
        const PAYLOAD: &[u8] =
            b"CactOS ping! 0123456789012345678901234567890123456789012";
        let repr = Icmpv4Repr::EchoRequest {
            ident: id,
            seq_no: seq,
            data: PAYLOAD,
        };
        let total = repr.buffer_len();
        let buf = match sock.send(total, dst) {
            Ok(b) => b,
            Err(_) => return false,
        };
        let mut pkt = Icmpv4Packet::new_unchecked(buf);
        let cap = ChecksumCapabilities::default();
        repr.emit(&mut pkt, &cap);
        true
    }
}

pub(crate) fn with_iface_sockets<R, F>(f: F) -> Option<R>
where
    F: FnOnce(&mut Interface, &mut SocketSet<'static>) -> R,
{
    let _stack_guard = StackGuard::new();
    // Holds `STACK_LOCK` for the whole closure body — the lock every other user
    // of `IFACE`/`SOCKET_SET` takes; the `Option` reads are tear-free and the
    // closure is not invoked at all when either is `None`.
    // SAFETY: `IFACE` is only used under `STACK_LOCK`, which this call holds, so
    // the borrow is exclusive for the closure body.
    let iface = (unsafe { (*core::ptr::addr_of_mut!(IFACE)).as_mut() })?;
    // SAFETY: as above, for `SOCKET_SET`.
    let socks = (unsafe { (*core::ptr::addr_of_mut!(SOCKET_SET)).as_mut() })?;
    Some(f(iface, socks))
}

/// Dequeue the echo reply matching `(id, seq)` from the kernel ICMP socket.
///
/// The socket is bound to `Ident(id)`, so smoltcp already filters by identifier;
/// the sequence check is what tells one probe from the next.  Returns
/// `(source address in host order, ICMP message length)`.
pub fn icmp_try_recv_reply(id: u16, seq: u16) -> Option<(u32, usize)> {
    let _stack_guard = StackGuard::new();
    // Holds `STACK_LOCK`; `ICMP_HANDLE`/`SOCKET_SET` are only read under that
    // lock (a stale read just yields `None`), and the ICMP socket is reached
    // through the shared `SocketSet`.
    {
        // SAFETY: `SOCKET_SET` is only used under `STACK_LOCK`, which this call
        // holds, so the borrow is exclusive for the rest of the function.
        let socks = (unsafe { (*core::ptr::addr_of_mut!(SOCKET_SET)).as_mut() })?;
        // SAFETY: `ICMP_HANDLE` is read under the stack lock; copying the
        // `Option` cannot tear.
        let handle = (unsafe { ICMP_HANDLE })?;
        let sock = socks.get_mut::<icmp::Socket>(handle);
        loop {
            let Ok((payload, addr)) = sock.recv() else {
                return None;
            };
            let Ok(pkt) = smoltcp::wire::Icmpv4Packet::new_checked(payload) else {
                continue;
            };
            let caps = smoltcp::phy::ChecksumCapabilities::ignored();
            let Ok(repr) = smoltcp::wire::Icmpv4Repr::parse(&pkt, &caps) else {
                continue;
            };
            if let smoltcp::wire::Icmpv4Repr::EchoReply { ident, seq_no, .. } = repr {
                if ident == id && seq_no == seq {
                    // Only IPv4 is compiled in, so the `_` arm is unreachable
                    // today; it stays so that enabling proto-ipv6 later does not
                    // silently report a v6 source as 0.0.0.0.
                    #[allow(unreachable_patterns)]
                    let src = match addr {
                        IpAddress::Ipv4(v4) => v4.to_bits(),
                        _ => 0,
                    };
                    return Some((src, payload.len()));
                }
            }
        }
    }
}
