//! Wire-format headers, socket limits, and C-layout structs shared with the NIC/VFS layers.

use core::ffi::{c_char, c_int, c_void};

pub const SKB_MAX_SIZE: usize = 2048;
pub const UDP_SOCK_MAX: usize = 8;
pub const UDP_RX_BUF_SIZE: usize = 4096;
pub const TCP_MAX_SOCKETS: usize = 8;
pub const TCP_RX_BUF_SIZE: usize = 4096;
pub const KSOCK_MAX: usize = 16;
pub const VFS_SOCKET: u32 = 0x06;
pub const VFS_POLLIN: u32 = 0x001;
pub const VFS_POLLOUT: u32 = 0x004;
pub const VFS_POLLERR: u32 = 0x008;
pub const VFS_POLLHUP: u32 = 0x010;
pub const VFS_POLLNVAL: u32 = 0x020;
pub const AF_INET: u16 = 2;

pub const KS_NONE: u32 = 0;
pub const KS_TCP: u32 = 1;
pub const KS_UDP: u32 = 2;

pub const SHUT_RD: c_int = 0;
pub const SHUT_WR: c_int = 1;
pub const SHUT_RDWR: c_int = 2;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct MacAddr {
    pub b: [u8; 6],
}

/* Wire headers.  Only the types `Skb` points at are kept: they document the
 * layout of each layer inside a received frame and mirror the C declarations in
 * `Cact/net/net.h` and the per-protocol headers.  Parsing itself is smoltcp's
 * job, so nothing here is dereferenced. */

#[repr(C, packed)]
pub struct EthHeader {
    pub dst: MacAddr,
    pub src: MacAddr,
    pub ethertype: u16,
}

#[repr(C, packed)]
pub struct ArpHeader {
    pub htype: u16,
    pub ptype: u16,
    pub hlen: u8,
    pub plen: u8,
    pub oper: u16,
    pub sha: MacAddr,
    pub spa: u32,
    pub tha: MacAddr,
    pub tpa: u32,
}

#[repr(C, packed)]
pub struct IpHeader {
    pub version_ihl: u8,
    pub tos: u8,
    pub total_len: u16,
    pub id: u16,
    pub flags_frag: u16,
    pub ttl: u8,
    pub protocol: u8,
    pub checksum: u16,
    pub src_ip: u32,
    pub dst_ip: u32,
}

#[repr(C, packed)]
pub struct IcmpHeader {
    pub type_: u8,
    pub code: u8,
    pub checksum: u16,
    pub id: u16,
    pub seq: u16,
}

#[repr(C, packed)]
pub struct UdpHeader {
    pub src_port: u16,
    pub dst_port: u16,
    pub length: u16,
    pub checksum: u16,
}

#[repr(C, packed)]
pub struct TcpHeader {
    pub src_port: u16,
    pub dst_port: u16,
    pub seq_num: u32,
    pub ack_num: u32,
    pub data_offset: u8,
    pub flags: u8,
    pub window: u16,
    pub checksum: u16,
    pub urgent_ptr: u16,
}

#[repr(C)]
pub struct Skb {
    pub data: [u8; SKB_MAX_SIZE],
    pub total_len: u16,
    pub data_offset: u16,
    pub eth: *mut EthHeader,
    pub arp: *mut ArpHeader,
    pub ip: *mut IpHeader,
    pub icmp: *mut IcmpHeader,
    pub udp: *mut UdpHeader,
    pub tcp: *mut TcpHeader,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NetDriver {
    pub mac: MacAddr,
    pub send: Option<extern "C" fn(*mut Skb) -> c_int>,
    pub poll: Option<extern "C" fn()>,
    pub get_mac: Option<extern "C" fn(*mut MacAddr)>,
    pub name: *const c_char,
}

// The kernel's semaphore and spinlock are `cact_sync`'s (`semaphore_t` /
// `spinlock_t`, shared with the scheduler and with the lock implementation
// itself).  Re-exported under the names this crate uses so there is exactly
// one Rust definition of each — hand-kept copies had already drifted once
// (a missing `count` field made `waiter_count` alias the next static).
pub use cact_sync::{semaphore_t as Semaphore, spinlock_t as Spinlock};

// The shared layout must stay the size the C side expects.
const _: () = assert!(core::mem::size_of::<Semaphore>() == 268);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct UdpSock {
    pub used: u8,
    pub local_port: u16,
    pub local_ip: u32,
    pub rx_buf: [u8; UDP_RX_BUF_SIZE],
    pub rx_len: u16,
    pub rx_ready: u8,
    pub last_src_ip: u32,
    pub last_src_port: u16,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct TcpSocket {
    pub used: u8,
    pub state: u32,
    pub local_ip: u32,
    pub local_port: u16,
    pub remote_ip: u32,
    pub remote_port: u16,
    pub snd_una: u32,
    pub snd_nxt: u32,
    pub snd_wnd: u32,
    pub rcv_nxt: u32,
    pub rcv_wnd: u32,
    pub rx_buf: [u8; TCP_RX_BUF_SIZE],
    pub rx_head: u16,
    pub rx_tail: u16,
    pub on_data: *mut c_void,
    pub on_event: *mut c_void,
    pub listen_parent: i8,
    pub accept_ready: u8,
    pub nodelay: u8,
    pub keepalive: u8,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Ksock {
    pub used: u8,
    pub kind: u32,
    pub proto_idx: c_int,
    pub shutdown_rd: u8,
    pub shutdown_wr: u8,
    pub so_reuseaddr: u8,
    pub so_keepalive: u8,
    pub tcp_nodelay: u8,
    pub so_error: c_int,
    /// O_NONBLOCK as set through fcntl(F_SETFL) on the socket fd.  Appended
    /// last so the C struct (`ksock_t` in `Cact/net/socket/socket.h`) and this
    /// mirror stay in step; `ksock_set_nonblock` is the only writer.
    pub nonblock: u8,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VfsOps {
    pub read: Option<extern "C" fn(*mut VfsNode, u32, u32, *mut c_char) -> c_int>,
    pub write: Option<extern "C" fn(*mut VfsNode, u32, u32, *mut c_char) -> c_int>,
    pub open: Option<extern "C" fn(*mut VfsNode)>,
    pub close: Option<extern "C" fn(*mut VfsNode)>,
    pub walk: *mut c_void,
    pub readdir: *mut c_void,
    pub listdir: *mut c_void,
    pub create: *mut c_void,
    pub delete: *mut c_void,
    pub mkdir: *mut c_void,
    pub rmdir: *mut c_void,
    pub rename: *mut c_void,
    pub symlink: *mut c_void,
    pub link: *mut c_void,
    pub unlink: *mut c_void,
    pub readlink: *mut c_void,
    pub ioctl: *mut c_void,
    // C's vfs_ops_t carries mmap_backing between ioctl and truncate.  Omitting
    // it shifted every later slot by one, so C's poll() read our `stat` entry
    // (NULL) and reported sockets as unpollable.
    pub mmap_backing: *mut c_void,
    pub truncate: *mut c_void,
    pub chmod: *mut c_void,
    pub chown: *mut c_void,
    pub mknod: *mut c_void,
    pub stat: *mut c_void,
    pub poll: Option<extern "C" fn(*mut VfsNode, u32) -> c_int>,
    pub lseek: *mut c_void,
}

// Slot positions are fixed by C's vfs_ops_t; rust_drm/src/vfs.rs asserts the
// same numbers for its own mirror.
const _: () = assert!(core::mem::size_of::<VfsOps>() == 100);
const _: () = assert!(core::mem::offset_of!(VfsOps, mmap_backing) == 68);
const _: () = assert!(core::mem::offset_of!(VfsOps, poll) == 92);

#[repr(C)]
pub struct VfsNode {
    pub name: [c_char; 128],
    pub type_: u32,
    pub size: u32,
    pub inode: u32,
    pub refcount: u32,
    pub mode: u32,
    pub uid: u32,
    pub gid: u32,
    pub ops: *mut VfsOps,
    // C's vfs_node_t has fops between ops and priv.  Without it priv_ landed on
    // C's fops slot: write_file_vfs()/read_file_vfs() then saw the Ksock* as a
    // vfs_file_ops_t and called its `write`/`read` — Ksock.kind == KS_TCP == 1,
    // i.e. a jump to linear address 1 and an immediate #UD.
    pub fops: *mut c_void,
    pub priv_: *mut c_void,
}

const _: () = assert!(core::mem::size_of::<VfsNode>() == 168);
const _: () = assert!(core::mem::offset_of!(VfsNode, type_) == 128);
const _: () = assert!(core::mem::offset_of!(VfsNode, ops) == 156);
const _: () = assert!(core::mem::offset_of!(VfsNode, fops) == 160);
const _: () = assert!(core::mem::offset_of!(VfsNode, priv_) == 164);

// ── Layout guards for the remaining C mirrors ─────────────────────────────
// Every struct below is declared twice — once in C (`ksock_t`, `tcp_socket_t`,
// `udp_sock_t`, `net_driver_t`, `skb_t`) and once here for the Rust code that
// writes it.  The EIP=3 crash of 2026-09-20 was exactly this class of drift, so
// each mirror gets the same guard as vfs_node_t: if someone inserts a field on
// one side, the build stops instead of the kernel jumping through a struct.
const _: () = assert!(core::mem::size_of::<Ksock>() == 28);
const _: () = assert!(core::mem::offset_of!(Ksock, kind) == 4);
const _: () = assert!(core::mem::offset_of!(Ksock, proto_idx) == 8);
const _: () = assert!(core::mem::offset_of!(Ksock, so_error) == 20);
const _: () = assert!(core::mem::offset_of!(Ksock, nonblock) == 24);

const _: () = assert!(core::mem::size_of::<TcpSocket>() == 4156);
const _: () = assert!(core::mem::offset_of!(TcpSocket, state) == 4);
const _: () = assert!(core::mem::offset_of!(TcpSocket, local_ip) == 8);
const _: () = assert!(core::mem::offset_of!(TcpSocket, remote_ip) == 16);
const _: () = assert!(core::mem::offset_of!(TcpSocket, rx_buf) == 44);
const _: () = assert!(core::mem::offset_of!(TcpSocket, rx_head) == 4140);
const _: () = assert!(core::mem::offset_of!(TcpSocket, on_data) == 4144);
const _: () = assert!(core::mem::offset_of!(TcpSocket, listen_parent) == 4152);

const _: () = assert!(core::mem::size_of::<UdpSock>() == 4116);
const _: () = assert!(core::mem::offset_of!(UdpSock, local_port) == 2);
const _: () = assert!(core::mem::offset_of!(UdpSock, local_ip) == 4);
const _: () = assert!(core::mem::offset_of!(UdpSock, rx_buf) == 8);
const _: () = assert!(core::mem::offset_of!(UdpSock, last_src_ip) == 4108);

const _: () = assert!(core::mem::size_of::<NetDriver>() == 24);
const _: () = assert!(core::mem::offset_of!(NetDriver, send) == 8);
const _: () = assert!(core::mem::offset_of!(NetDriver, name) == 20);

const _: () = assert!(core::mem::size_of::<Skb>() == 2076);
const _: () = assert!(core::mem::offset_of!(Skb, total_len) == 2048);
const _: () = assert!(core::mem::offset_of!(Skb, eth) == 2052);
const _: () = assert!(core::mem::offset_of!(Skb, tcp) == 2072);

#[repr(C)]
pub struct SockAddrIn {
    pub sin_family: u16,
    pub sin_port: u16,
    pub sin_addr: u32,
    pub sin_zero: [u8; 8],
}

pub const TCP_CLOSED: u32 = 0;
pub const TCP_LISTEN: u32 = 1;
pub const TCP_SYN_SENT: u32 = 2;
pub const TCP_SYN_RECEIVED: u32 = 3;
pub const TCP_ESTABLISHED: u32 = 4;
pub const TCP_FIN_WAIT_1: u32 = 5;
pub const TCP_FIN_WAIT_2: u32 = 6;
pub const TCP_CLOSE_WAIT: u32 = 7;
pub const TCP_CLOSING: u32 = 8;
pub const TCP_LAST_ACK: u32 = 9;
pub const TCP_TIME_WAIT: u32 = 10;

/// `TcpSocket::state` mirrors smoltcp's `tcp::State` discriminants and the C
/// `tcp_state_t` enum, so the cached value can be compared against these.
const _: () = assert!(TCP_CLOSE_WAIT == smoltcp::socket::tcp::State::CloseWait as u32);
const _: () = assert!(TCP_TIME_WAIT == smoltcp::socket::tcp::State::TimeWait as u32);
const _: () = assert!(TCP_ESTABLISHED == smoltcp::socket::tcp::State::Established as u32);
