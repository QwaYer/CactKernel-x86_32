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

const PHY_MTU: usize = 1536;
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
    rx: [u8; PHY_MTU],
    rx_len: usize,
    rx_pending: bool,
    tx: [u8; PHY_MTU],
}

fn active_nic_ptr() -> *mut crate::types::NetDriver {
    unsafe {
        AtomicPtr::from_ptr(core::ptr::addr_of_mut!(runtime::active_nic))
            .load(Ordering::Acquire)
    }
}

impl CactPhy {
    const fn new() -> Self {
        Self {
            rx: [0; PHY_MTU],
            rx_len: 0,
            rx_pending: false,
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
        unsafe {
            let nic = active_nic_ptr();
            if !nic.is_null() {
                let skb = skb::skb_alloc();
                if !skb.is_null() {
                    let p = skb::skb_put(skb, len as u16);
                    if !p.is_null() {
                        core::ptr::copy_nonoverlapping(self.buf.as_ptr(), p, len);
                        if let Some(send) = (*nic).send {
                            let _ = send(skb);
                        } else {
                            skb::kfree_skb(skb);
                        }
                    } else {
                        skb::kfree_skb(skb);
                    }
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
        if !self.rx_pending {
            return None;
        }
        self.rx_pending = false;
        let n = self.rx_len.min(self.rx.len());
        Some((
            CactRxToken {
                slice: &self.rx[..n],
            },
            CactTxToken { buf: &mut self.tx },
        ))
    }

    fn transmit(&mut self, _timestamp: Instant) -> Option<Self::TxToken<'_>> {
        Some(CactTxToken { buf: &mut self.tx })
    }

    fn capabilities(&self) -> DeviceCapabilities {
        let mut c = DeviceCapabilities::default();
        c.max_transmission_unit = PHY_MTU;
        c.max_burst_size = Some(1);
        c.medium = Medium::Ethernet;
        c
    }
}

/// Copy one Ethernet frame from driver `Skb` into the PHY RX staging buffer and wake `net_poll_task`.
pub fn stack_enqueue_rx(skb: *mut Skb) {
    if skb.is_null() {
        return;
    }
    unsafe {
        let len = skb::skb_len(skb) as usize;
        if len == 0 || len > PHY_MTU {
            skb::kfree_skb(skb);
            return;
        }
        let src = skb::skb_data(skb);
        if !STACK_READY {
            skb::kfree_skb(skb);
            return;
        }
        core::ptr::copy_nonoverlapping(src, PHY.rx.as_mut_ptr(), len);
        PHY.rx_len = len;
        PHY.rx_pending = true;
        skb::kfree_skb(skb);
        ffi_kernel::up(core::ptr::addr_of_mut!(runtime::net_sema));
    }
}

pub unsafe fn stack_teardown() {
    if let Some(ref mut socks) = SOCKET_SET {
        crate::dns_resolve::remove_socket(socks);
    }
    IFACE = None;
    SOCKET_SET = None;
    ICMP_HANDLE = None;
    ICMP_IDENT_BOUND = 0xFFFF;
    for s in SOCKET_STORAGE.iter_mut() {
        *s = SocketStorage::EMPTY;
    }
    PHY = CactPhy::new();
    STACK_READY = false;
    crate::tcp::reset_tcp_smoltcp_state();
    crate::udp::reset_udp_smoltcp_state();
}

pub fn stack_init() {
    unsafe {
        if STACK_READY {
            return;
        }
        let mac = runtime::my_mac.b;
        let eth = EthernetAddress::from_bytes(&mac);
        let mut cfg = Config::new(HardwareAddress::Ethernet(eth));
        cfg.random_seed = u64::from(ffi_kernel::timer_ticks_get());
        let now = ticks_to_instant(ffi_kernel::timer_ticks_get());
        IFACE = Some(Interface::new(cfg, &mut PHY, now));
        SOCKET_SET = Some(SocketSet::new(&mut SOCKET_STORAGE[..]));

        let iface = IFACE.as_mut().unwrap();
        sync_iface_ipv4_from_config(iface);

        let socks = SOCKET_SET.as_mut().unwrap();
        let icmp_rx = icmp::PacketBuffer::new(&mut ICMP_RX_META[..], &mut ICMP_RX_PAYLOAD[..]);
        let icmp_tx = icmp::PacketBuffer::new(&mut ICMP_TX_META[..], &mut ICMP_TX_PAYLOAD[..]);
        let icmp_sock = icmp::Socket::new(icmp_rx, icmp_tx);
        ICMP_HANDLE = Some(socks.add(icmp_sock));

        crate::dns_resolve::init_socket(socks);

        STACK_READY = true;
        ffi_kernel::klog_static(
            ffi_kernel::LOG_OK,
            b"smoltcp interface and sockets ready (poll from net_poll_task)\0",
        );
    }
}

pub fn stack_poll() {
    unsafe {
        let nic = active_nic_ptr();
        if let Some(poll) = (!nic.is_null())
            .then(|| (*nic).poll)
            .flatten()
        {
            poll();
        }
        if !STACK_READY {
            return;
        }
        let now = ticks_to_instant(ffi_kernel::timer_ticks_get());
        if let (Some(ref mut iface), Some(ref mut socks)) = (IFACE.as_mut(), SOCKET_SET.as_mut()) {
            iface.poll(now, &mut PHY, socks);
            crate::tcp::sync_tcp_pcbs_from_smoltcp(iface, socks);
            crate::udp::sync_udp_pcbs_from_smoltcp(socks);
        }
    }
}

/// Fire-and-forget ICMPv4 echo request (kernel ping helper).
pub fn icmp_echo_request_host(dst_ip_host: u32, id: u16, seq: u16) -> bool {
    unsafe {
        if !STACK_READY {
            return false;
        }
        let Some(icmp_h) = ICMP_HANDLE else {
            return false;
        };
        let Some(ref mut iface) = IFACE.as_mut() else {
            return false;
        };
        let Some(ref mut socks) = SOCKET_SET.as_mut() else {
            return false;
        };
        let need_replace = {
            let s = socks.get_mut::<icmp::Socket>(icmp_h);
            s.is_open() && ICMP_IDENT_BOUND != id
        };
        if need_replace {
            let _removed = socks.remove(icmp_h);
            core::mem::drop(_removed);
            let icmp_rx = icmp::PacketBuffer::new(&mut ICMP_RX_META[..], &mut ICMP_RX_PAYLOAD[..]);
            let icmp_tx = icmp::PacketBuffer::new(&mut ICMP_TX_META[..], &mut ICMP_TX_PAYLOAD[..]);
            let icmp_sock = icmp::Socket::new(icmp_rx, icmp_tx);
            ICMP_HANDLE = Some(socks.add(icmp_sock));
            ICMP_IDENT_BOUND = 0xFFFF;
        }
        let icmp_h = ICMP_HANDLE.unwrap();
        let sock = socks.get_mut::<icmp::Socket>(icmp_h);
        if !sock.is_open() {
            if sock.bind(icmp::Endpoint::Ident(id)).is_err() {
                return false;
            }
            ICMP_IDENT_BOUND = id;
        }
        let dst = IpAddress::Ipv4(ipv4_from_host(dst_ip_host));
        use smoltcp::phy::ChecksumCapabilities;
        use smoltcp::wire::{Icmpv4Packet, Icmpv4Repr};
        const PAYLOAD: &[u8] = b"CactOS ping!";
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
        let _ = iface.context();
        true
    }
}

pub(crate) fn with_iface_sockets<R, F>(f: F) -> Option<R>
where
    F: FnOnce(&mut Interface, &mut SocketSet<'static>) -> R,
{
    unsafe {
        let iface = IFACE.as_mut()?;
        let socks = SOCKET_SET.as_mut()?;
        Some(f(iface, socks))
    }
}
