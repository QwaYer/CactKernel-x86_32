use core::ffi::{c_char, c_int, c_void};

pub const SKB_MAX_SIZE: usize = 2048;
pub const ARP_CACHE_SIZE: usize = 16;
pub const UDP_SOCK_MAX: usize = 8;
pub const UDP_RX_BUF_SIZE: usize = 4096;
pub const TCP_MAX_SOCKETS: usize = 8;
pub const TCP_RX_BUF_SIZE: usize = 4096;
pub const KSOCK_MAX: usize = 16;
pub const VFS_SOCKET: u32 = 0x06;
pub const AF_INET: u16 = 2;

pub const ETH_TYPE_IPV4: u16 = 0x0800;
pub const ETH_TYPE_ARP: u16 = 0x0806;
pub const IP_PROTO_ICMP: u8 = 1;
pub const IP_PROTO_TCP: u8 = 6;
pub const IP_PROTO_UDP: u8 = 17;

pub const ICMP_ECHO_REQUEST: u8 = 8;
pub const ICMP_ECHO_REPLY: u8 = 0;

pub const TCP_FIN: u8 = 0x01;
pub const TCP_SYN: u8 = 0x02;
pub const TCP_RST: u8 = 0x04;
pub const TCP_PSH: u8 = 0x08;
pub const TCP_ACK: u8 = 0x10;

pub const KS_NONE: u32 = 0;
pub const KS_TCP: u32 = 1;
pub const KS_UDP: u32 = 2;

pub const SO_REUSEADDR: c_int = 2;
pub const SO_KEEPALIVE: c_int = 9;
pub const SO_ERROR: c_int = 4;
pub const TCP_NODELAY: c_int = 1;
pub const SOL_SOCKET: c_int = 1;
pub const IPPROTO_TCP_C: c_int = 6;

pub const SHUT_RD: c_int = 0;
pub const SHUT_WR: c_int = 1;
pub const SHUT_RDWR: c_int = 2;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct MacAddr {
    pub b: [u8; 6],
}

pub const MAC_BROADCAST: MacAddr = MacAddr { b: [0xFF; 6] };

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

#[repr(C)]
pub struct Spinlock {
    pub locked: u32,
}

#[repr(C)]
pub struct Semaphore {
    pub guard: Spinlock,
    pub waiters: [*mut c_void; 64],
    pub waiter_count: u32,
}

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
}

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
    pub priv_: *mut c_void,
}

#[repr(C)]
pub struct SockAddrIn {
    pub sin_family: u16,
    pub sin_port: u16,
    pub sin_addr: u32,
    pub sin_zero: [u8; 8],
}

#[repr(C)]
pub struct SendToArgs {
    pub fd: c_int,
    pub buf: *const c_void,
    pub len: u32,
    pub flags: c_int,
    pub dest: *const SockAddrIn,
    pub addrlen: u32,
}

#[repr(C)]
pub struct RecvFromArgs {
    pub fd: c_int,
    pub buf: *mut c_void,
    pub len: u32,
    pub flags: c_int,
    pub src: *mut SockAddrIn,
    pub addrlen: *mut u32,
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
