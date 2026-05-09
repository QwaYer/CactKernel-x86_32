use crate::config;
use crate::ffi_kernel;
use crate::udp;
use crate::types::UDP_RX_BUF_SIZE;

const DHCP_CLIENT_PORT: u16 = 68;
const DHCP_SERVER_PORT: u16 = 67;
const DHCP_MAGIC: u32 = 0x6382_5363;
const DHCP_OPT_SUBNET: u8 = 1;
const DHCP_OPT_ROUTER: u8 = 3;
const DHCP_OPT_DNS: u8 = 6;
const DHCP_OPT_REQ_IP: u8 = 50;
const DHCP_OPT_LEASE_TIME: u8 = 51;
const DHCP_OPT_MSG_TYPE: u8 = 53;
const DHCP_OPT_SERVER_ID: u8 = 54;
const DHCP_OPT_PARAM_REQ: u8 = 55;
const DHCP_OPT_T1: u8 = 58;
const DHCP_OPT_T2: u8 = 59;
const DHCP_OPT_END: u8 = 255;
const DHCP_MSG_REQUEST: u8 = 3;
const DHCP_MSG_ACK: u8 = 5;

#[repr(C, packed)]
struct DhcpPacket {
    op: u8,
    htype: u8,
    hlen: u8,
    hops: u8,
    xid: u32,
    secs: u16,
    flags: u16,
    ciaddr: u32,
    yiaddr: u32,
    siaddr: u32,
    giaddr: u32,
    chaddr: [u8; 16],
    sname: [u8; 64],
    file: [u8; 128],
    magic: u32,
    opts: [u8; 312],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct DhcpLeaseCfg {
    pub ip_host: u32,
    pub netmask_host: u32,
    pub gateway_host: u32,
    pub dns_host: u32,
    pub server_host: u32,
    pub lease_s: u32,
    pub t1_s: u32,
    pub t2_s: u32,
}

static mut LEASE: DhcpLeaseCfg = DhcpLeaseCfg {
    ip_host: 0,
    netmask_host: 0,
    gateway_host: 0,
    dns_host: 0,
    server_host: 0,
    lease_s: 0,
    t1_s: 0,
    t2_s: 0,
};
static mut LEASE_ACTIVE: u8 = 0;
static mut LEASE_T1_DEADLINE: u32 = 0;
static mut LEASE_T2_DEADLINE: u32 = 0;
static mut LEASE_EXP_DEADLINE: u32 = 0;
static mut LEASE_LAST_RENEW_TRY: u32 = 0;

fn opt_u32_host(opts: &[u8], key: u8) -> Option<u32> {
    let mut i = 0usize;
    while i < opts.len() {
        let t = opts[i];
        i += 1;
        if t == DHCP_OPT_END {
            break;
        }
        if t == 0 {
            continue;
        }
        if i >= opts.len() {
            break;
        }
        let l = opts[i] as usize;
        i += 1;
        if i + l > opts.len() {
            break;
        }
        if t == key && l >= 4 {
            let netv = ((opts[i] as u32) << 24)
                | ((opts[i + 1] as u32) << 16)
                | ((opts[i + 2] as u32) << 8)
                | (opts[i + 3] as u32);
            return Some(u32::from_be(netv));
        }
        i += l;
    }
    None
}

fn opt_u8(opts: &[u8], key: u8) -> Option<u8> {
    let mut i = 0usize;
    while i < opts.len() {
        let t = opts[i];
        i += 1;
        if t == DHCP_OPT_END {
            break;
        }
        if t == 0 {
            continue;
        }
        if i >= opts.len() {
            break;
        }
        let l = opts[i] as usize;
        i += 1;
        if i + l > opts.len() {
            break;
        }
        if t == key && l >= 1 {
            return Some(opts[i]);
        }
        i += l;
    }
    None
}

unsafe fn apply_lease(cfg: DhcpLeaseCfg) {
    let _ = config::rust_net_set_ipv4_config(cfg.ip_host, cfg.netmask_host, cfg.gateway_host, cfg.dns_host);
    LEASE = cfg;
    LEASE_ACTIVE = 1;
    let now = ffi_kernel::timer_ticks_get();
    let tick_hz = 100u32;
    let lease = if cfg.lease_s == 0 { 3600 } else { cfg.lease_s };
    let t1 = if cfg.t1_s == 0 { lease / 2 } else { cfg.t1_s };
    let t2 = if cfg.t2_s == 0 { (lease * 7) / 8 } else { cfg.t2_s };
    LEASE_T1_DEADLINE = now.wrapping_add(t1.saturating_mul(tick_hz));
    LEASE_T2_DEADLINE = now.wrapping_add(t2.saturating_mul(tick_hz));
    LEASE_EXP_DEADLINE = now.wrapping_add(lease.saturating_mul(tick_hz));
    LEASE_LAST_RENEW_TRY = 0;
    ffi_kernel::c_kprint(b"[RUST-NET][DHCP] lease applied ip=\0");
    ffi_kernel::c_kprint_hex(cfg.ip_host);
    ffi_kernel::c_kprint(b" srv=\0");
    ffi_kernel::c_kprint_hex(cfg.server_host);
    ffi_kernel::c_kprint(b" lease=\0");
    ffi_kernel::c_kprint_hex(lease);
    ffi_kernel::c_kprint(b" t1=\0");
    ffi_kernel::c_kprint_hex(t1);
    ffi_kernel::c_kprint(b" t2=\0");
    ffi_kernel::c_kprint_hex(t2);
    ffi_kernel::c_kprint(b"\n\0");
}

unsafe fn try_renew(broadcast: bool) -> i32 {
    ffi_kernel::c_kprint(b"[RUST-NET][DHCP] renew start mode=\0");
    ffi_kernel::c_kprint_hex(if broadcast { 2 } else { 1 });
    ffi_kernel::c_kprint(b"\n\0");
    if LEASE_ACTIVE == 0 || LEASE.ip_host == 0 {
        return -1;
    }

    let sock = udp::udp_sock_alloc();
    if sock < 0 {
        return -1;
    }
    udp::udp_socks[sock as usize].local_port = DHCP_CLIENT_PORT;
    udp::udp_socks[sock as usize].local_ip = LEASE.ip_host;

    let mut req = DhcpPacket {
        op: 1,
        htype: 1,
        hlen: 6,
        hops: 0,
        xid: (ffi_kernel::timer_ticks_get() ^ 0xD4A7_0000).to_be(),
        secs: 0,
        flags: if broadcast { 0x8000u16.to_be() } else { 0 },
        ciaddr: LEASE.ip_host.to_be(),
        yiaddr: 0,
        siaddr: 0,
        giaddr: 0,
        chaddr: [0; 16],
        sname: [0; 64],
        file: [0; 128],
        magic: DHCP_MAGIC.to_be(),
        opts: [0; 312],
    };

    let mut oi = 0usize;
    req.opts[oi] = DHCP_OPT_MSG_TYPE;
    oi += 1;
    req.opts[oi] = 1;
    oi += 1;
    req.opts[oi] = DHCP_MSG_REQUEST;
    oi += 1;
    req.opts[oi] = DHCP_OPT_REQ_IP;
    oi += 1;
    req.opts[oi] = 4;
    oi += 1;
    let rip = LEASE.ip_host.to_be();
    req.opts[oi] = ((rip >> 24) & 0xFF) as u8;
    req.opts[oi + 1] = ((rip >> 16) & 0xFF) as u8;
    req.opts[oi + 2] = ((rip >> 8) & 0xFF) as u8;
    req.opts[oi + 3] = (rip & 0xFF) as u8;
    oi += 4;
    if LEASE.server_host != 0 {
        req.opts[oi] = DHCP_OPT_SERVER_ID;
        oi += 1;
        req.opts[oi] = 4;
        oi += 1;
        let sid = LEASE.server_host.to_be();
        req.opts[oi] = ((sid >> 24) & 0xFF) as u8;
        req.opts[oi + 1] = ((sid >> 16) & 0xFF) as u8;
        req.opts[oi + 2] = ((sid >> 8) & 0xFF) as u8;
        req.opts[oi + 3] = (sid & 0xFF) as u8;
        oi += 4;
    }
    req.opts[oi] = DHCP_OPT_PARAM_REQ;
    oi += 1;
    req.opts[oi] = 3;
    oi += 1;
    req.opts[oi] = DHCP_OPT_SUBNET;
    oi += 1;
    req.opts[oi] = DHCP_OPT_ROUTER;
    oi += 1;
    req.opts[oi] = DHCP_OPT_DNS;
    oi += 1;
    req.opts[oi] = DHCP_OPT_END;

    let dst_h = if broadcast || LEASE.server_host == 0 {
        0xFFFF_FFFFu32
    } else {
        LEASE.server_host
    };
    let _ = udp::udp_sock_send(
        sock,
        dst_h.to_be(),
        DHCP_SERVER_PORT,
        (&req as *const DhcpPacket).cast::<u8>(),
        core::mem::size_of::<DhcpPacket>() as u16,
    );

    let mut buf = [0u8; UDP_RX_BUF_SIZE];
    for _ in 0..50 {
        let mut src_ip = 0u32;
        let mut src_port = 0u16;
        let n = udp::udp_sock_recv(
            sock,
            buf.as_mut_ptr(),
            buf.len() as u16,
            core::ptr::addr_of_mut!(src_ip),
            core::ptr::addr_of_mut!(src_port),
        );
        if n <= 0 {
            ffi_kernel::sched_sleep_ticks(2);
            continue;
        }
        let min = core::mem::size_of::<DhcpPacket>() as i32 - 312;
        if n < min {
            continue;
        }
        let rp = &*(buf.as_ptr() as *const DhcpPacket);
        if rp.op != 2 || rp.xid != req.xid || u32::from_be(rp.magic) != DHCP_MAGIC {
            continue;
        }
        let opts = &rp.opts[..];
        let mtype = opt_u8(opts, DHCP_OPT_MSG_TYPE).unwrap_or(0);
        if mtype != DHCP_MSG_ACK {
            continue;
        }
        let mut cfg = LEASE;
        cfg.ip_host = u32::from_be(rp.yiaddr);
        cfg.netmask_host = opt_u32_host(opts, DHCP_OPT_SUBNET).unwrap_or(cfg.netmask_host);
        cfg.gateway_host = opt_u32_host(opts, DHCP_OPT_ROUTER).unwrap_or(cfg.gateway_host);
        cfg.dns_host = opt_u32_host(opts, DHCP_OPT_DNS).unwrap_or(cfg.dns_host);
        cfg.server_host = opt_u32_host(opts, DHCP_OPT_SERVER_ID).unwrap_or(cfg.server_host);
        cfg.lease_s = opt_u32_host(opts, DHCP_OPT_LEASE_TIME).unwrap_or(cfg.lease_s);
        cfg.t1_s = opt_u32_host(opts, DHCP_OPT_T1).unwrap_or(cfg.lease_s / 2);
        cfg.t2_s = opt_u32_host(opts, DHCP_OPT_T2).unwrap_or((cfg.lease_s * 7) / 8);
        apply_lease(cfg);
        udp::udp_sock_free(sock);
        ffi_kernel::c_kprint(b"[RUST-NET][DHCP] lease renewed\n\0");
        return 0;
    }
    udp::udp_sock_free(sock);
    -1
}

extern "C" fn dhcp_renew_daemon() {
    ffi_kernel::c_kprint(b"[RUST-NET][DHCP] daemon started\n\0");
    loop {
        unsafe {
            if LEASE_ACTIVE != 0 {
                let now = ffi_kernel::timer_ticks_get();
                if now >= LEASE_EXP_DEADLINE {
                    LEASE_ACTIVE = 0;
                    ffi_kernel::c_kprint(b"[RUST-NET][DHCP] lease expired\n\0");
                } else if now >= LEASE_T1_DEADLINE {
                    if now.wrapping_sub(LEASE_LAST_RENEW_TRY) >= 500 {
                        LEASE_LAST_RENEW_TRY = now;
                        let mut ok = try_renew(false) == 0;
                        if !ok && now >= LEASE_T2_DEADLINE {
                            ok = try_renew(true) == 0;
                        }
                        if !ok {
                            ffi_kernel::c_kprint(b"[RUST-NET][DHCP] renew retry\n\0");
                        }
                    }
                }
            }
            ffi_kernel::sched_sleep_ticks(100);
        }
    }
}

#[no_mangle]
pub extern "C" fn rust_net_dhcp_set_lease(cfg: *const DhcpLeaseCfg) -> i32 {
    if cfg.is_null() {
        return -1;
    }
    unsafe {
        let mut c = *cfg;
        if c.lease_s == 0 {
            c.lease_s = 3600;
        }
        if c.t1_s == 0 {
            c.t1_s = c.lease_s / 2;
        }
        if c.t2_s == 0 {
            c.t2_s = (c.lease_s * 7) / 8;
        }
        apply_lease(c);
    }
    0
}

#[no_mangle]
pub extern "C" fn rust_net_dhcp_start_daemon() {
    unsafe {
        let _ = ffi_kernel::create_task(dhcp_renew_daemon);
    }
}
