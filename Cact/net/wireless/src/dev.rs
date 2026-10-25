//! The 802.11 station engine state and protocol logic.
//!
//! One radio (the board has one Wi-Fi dongle), so the engine keeps a single
//! `Dev`.  Radio access and every read/write of `Dev` are serialised by the
//! sleeping mutex `RADIO_LOCK`: the contexts are the netdev poll task (data RX,
//! beacon liveness), the L3 stack's transmit path, and a userspace ioctl
//! (`scan` / `connect` / `eapol_rx`).  A mutex, not a spinlock — the guarded
//! sections contain USB bulk transfers that can take up to ~1 s (a TX-status
//! wait), and spinning would stall other CPUs with interrupts disabled.
//!
//! A received frame never re-enters this state through a reference: the driver's
//! RX callback only drops the MPDU into `RXQ`, and the frame is dispatched
//! afterwards from the same (locked) polling loop.

use cact_net::runtime::{netif_rx, register_netdev, unregister_netdev};
use cact_net::skb::{kfree_skb, skb_alloc, skb_data, skb_len, skb_put};
use cact_net::types::{MacAddr, NetDriver, Skb};

use crate::control;
use crate::frame::{self, Bss, DOT11_HDR, LLC_SNAP_LEN};
use crate::hw::{self, Cact80211Conf, Cact80211Hw, Q_DATA};

// ── limits and errors ────────────────────────────────────────────────────────
const MAX_BSS: usize = 64;
const RX_RING: usize = 8;
const RX_FRAME_MAX: usize = 2312;
const EAPOL_Q: usize = 8;
const EAPOL_MAX: usize = 512;
const SCRATCH_MAX: usize = 2312;

const EINVAL: i32 = -22;
const ENODEV: i32 = -19;
const ENOENT: i32 = -2;
const EIO: i32 = -5;
const EAGAIN: i32 = -11;
const ETIMEDOUT: i32 = -110;

// Scan dwell (as the retired in-driver scan): re-probe on a busy channel, leave
// once the channel goes quiet.  A driver RX poll blocks ~20 ms when empty.
const SCAN_POLLS_MAX: u32 = 50;
const SCAN_QUIET: u32 = 5;
const SCAN_REPROBE: u32 = 10;
const CTS_SETTLE_US: u64 = 5_000;
const MGMT_TIMEOUT_US: u64 = 1_000_000;
const BEACON_LOSS_US: u64 = 3_000_000;

// ── received-frame mailbox (driver callback → engine) ─────────────────────────
struct RxRing {
    len: [u16; RX_RING],
    data: [[u8; RX_FRAME_MAX]; RX_RING],
    head: usize,
    tail: usize,
}

static mut RXQ: RxRing = RxRing {
    len: [0; RX_RING],
    data: [[0; RX_FRAME_MAX]; RX_RING],
    head: 0,
    tail: 0,
};

static mut RX_SCRATCH: [u8; SCRATCH_MAX] = [0; SCRATCH_MAX];

/// Serialises every radio access and every read/write of `Dev`.  A sleeping
/// mutex (not a spinlock): the guarded sections include USB bulk transfers with
/// timeouts, and the mutex cooperates with the scheduler instead of holding
/// interrupts off across a transfer.  It is recursive, which matters because the
/// stack's transmit path runs inside RX delivery (`netif_rx` → smoltcp → send).
static mut RADIO_LOCK: cact_sync::mutex_t = cact_sync::mutex_t {
    locked: core::sync::atomic::AtomicU32::new(0),
    guard: cact_sync::spinlock_t::new(),
    owner: core::ptr::null_mut(),
    waiters: [core::ptr::null_mut(); cact_sync::MUTEX_WAIT_QUEUE_MAX],
    waiter_count: 0,
    recursion_count: 0,
};

fn radio_lock() {
    // SAFETY: `RADIO_LOCK` is the engine's kernel-lifetime mutex; every acquirer
    // goes through this pair.
    unsafe { cact_sync::mutex_lock(core::ptr::addr_of_mut!(RADIO_LOCK)) };
}

fn radio_unlock() {
    // SAFETY: releases a mutex this task holds (paired with `radio_lock`).
    unsafe { cact_sync::mutex_unlock(core::ptr::addr_of_mut!(RADIO_LOCK)) };
}

fn rx_enqueue(f: &[u8]) {
    if f.is_empty() || f.len() > RX_FRAME_MAX {
        return;
    }
    // SAFETY: `RXQ` is the engine's kernel-lifetime mailbox; the driver callback
    // is the single producer and runs on the polling thread.
    let q = unsafe { &mut *core::ptr::addr_of_mut!(RXQ) };
    let next = (q.head + 1) % RX_RING;
    if next == q.tail {
        return; // full: drop
    }
    q.data[q.head][..f.len()].copy_from_slice(f);
    q.len[q.head] = f.len() as u16;
    q.head = next;
}

// ── state ─────────────────────────────────────────────────────────────────────
pub struct Dev {
    pub hw: *mut Cact80211Hw,
    pub name_ready: bool,
    pub channel: u8,
    pub assoc: bool,
    pub secured: bool,
    pub privacy: bool,
    pub bssid: [u8; 6],
    pub our_mac: [u8; 6],
    pub bss: [Bss; MAX_BSS],
    pub bss_count: usize,

    pub tk: [u8; 16],
    pub gtk: [u8; 16],
    pub have_tk: bool,
    pub have_gtk: bool,
    pub gtk_id: u8,
    pub pn: [u64; 4],

    auth_status: i32,
    assoc_status: i32,
    last_beacon_us: u64,
    pub last_error: i32,

    eapol: [[u8; EAPOL_MAX]; EAPOL_Q],
    eapol_src: [[u8; 6]; EAPOL_Q],
    eapol_len: [u16; EAPOL_Q],
    eapol_head: usize,
    eapol_tail: usize,

    pub tx_data: u32,
    pub tx_group: u32,
    pub tx_fail: u32,
    pub tx_drop_noassoc: u32,
    pub tx_drop_nokey: u32,
    pub rx_data: u32,
    pub rx_group: u32,
    pub rx_bad: u32,
    pub rx_nokey: u32,
    pub rx_other: u32,
    pub beacon_rx: u32,
    pub deauth_rx: u32,
}

impl Dev {
    const fn new() -> Self {
        Dev {
            hw: core::ptr::null_mut(),
            name_ready: false,
            channel: 1,
            assoc: false,
            secured: false,
            privacy: false,
            bssid: [0; 6],
            our_mac: [0; 6],
            bss: [Bss::empty(); MAX_BSS],
            bss_count: 0,
            tk: [0; 16],
            gtk: [0; 16],
            have_tk: false,
            have_gtk: false,
            gtk_id: 0,
            pn: [0; 4],
            auth_status: -1,
            assoc_status: -1,
            last_beacon_us: 0,
            last_error: 0,
            eapol: [[0; EAPOL_MAX]; EAPOL_Q],
            eapol_src: [[0; 6]; EAPOL_Q],
            eapol_len: [0; EAPOL_Q],
            eapol_head: 0,
            eapol_tail: 0,
            tx_data: 0,
            tx_group: 0,
            tx_fail: 0,
            tx_drop_noassoc: 0,
            tx_drop_nokey: 0,
            rx_data: 0,
            rx_group: 0,
            rx_bad: 0,
            rx_nokey: 0,
            rx_other: 0,
            beacon_rx: 0,
            deauth_rx: 0,
        }
    }
}

static mut DEV: Dev = Dev::new();

fn dev() -> &'static mut Dev {
    // SAFETY: the engine's single state object.  Every accessor holds
    // `RADIO_LOCK`, which serialises the poll task, the transmit path and the
    // ioctls, so no two callers of `dev()` are live at once (recursive
    // acquisition by the RX→stack→send path is the same task).
    unsafe { &mut *core::ptr::addr_of_mut!(DEV) }
}

fn now_us() -> u64 {
    // SAFETY: a kernel time service with no preconditions.
    unsafe { crate::ffi::ktime_get_usec() }
}

fn sleep_us(us: u64) {
    // SAFETY: a kernel delay service with no preconditions.
    unsafe { crate::ffi::ktime_busy_wait_us(us) };
}

// ── registration ──────────────────────────────────────────────────────────────
static mut NETDEV: NetDriver = NetDriver {
    mac: MacAddr { b: [0; 6] },
    send: Some(net_send),
    poll: Some(net_poll),
    get_mac: None,
    name: b"wlan0\0".as_ptr() as *const core::ffi::c_char,
};

pub fn register(hw: *mut Cact80211Hw) -> i32 {
    if hw.is_null() {
        return EINVAL;
    }
    let d = dev();
    if !d.hw.is_null() {
        unregister(d.hw); // replace the previous radio
    }
    d.hw = hw;
    d.bss_count = 0;

    // SAFETY: `hw` is non-null (checked) and points at the driver's live config.
    let mac = unsafe { (*hw).mac };
    d.our_mac = mac;

    // Bring the control node up first so userspace can see errors through
    // CACT_WLANCTL_STATUS even when the radio init below is incomplete.
    control::register_node();

    // SAFETY: `NETDEV` is the engine's kernel-lifetime NIC descriptor; every
    // field is initialised above.
    let nd = core::ptr::addr_of_mut!(NETDEV);
    unsafe { (*nd).mac = MacAddr { b: mac } };
    // SAFETY: `register_netdev` takes a live `NetDriver`; this runs on the
    // driver's bring-up path.
    unsafe { register_netdev(nd) };
    d.name_ready = true;

    0
}

pub fn unregister(hw: *mut Cact80211Hw) {
    let d = dev();
    if hw.is_null() || d.hw != hw {
        return;
    }
    if d.name_ready {
        // SAFETY: `NETDEV` is the descriptor registered above; teardown path.
        unregister_netdev(core::ptr::addr_of_mut!(NETDEV));
        d.name_ready = false;
    }
    control::unregister_node();
    d.hw = core::ptr::null_mut();
    d.assoc = false;
    d.secured = false;
}

// ── low-level radio helpers ───────────────────────────────────────────────────
fn apply_config(d: &mut Dev, assoc: bool) {
    let bssid = d.bssid;
    let mut basic = 0u16;
    let mut erp = 0u16;
    if let Some(b) = find_bss(&d.bss[..d.bss_count], &bssid) {
        basic = b.basic_rates;
        erp = b.erp_flags;
    }
    let conf = Cact80211Conf {
        channel: d.channel,
        assoc: assoc as u8,
        bssid,
        basic_rates: basic,
        erp_flags: erp,
    };
    // SAFETY: `d.hw` is live while registered.
    unsafe { hw::radio_config(d.hw, &conf) };
}

/// Drive the radio for one poll and dispatch everything it delivered.
fn pump(d: &mut Dev) -> usize {
    if d.hw.is_null() {
        return 0;
    }
    // SAFETY: `d.hw` is live while registered.
    unsafe { hw::radio_rx_poll(d.hw) };
    drain(d)
}

fn drain(d: &mut Dev) -> usize {
    let mut n = 0;
    loop {
        // SAFETY: `RXQ` is the engine's mailbox; the consumer runs here.
        let q = unsafe { &mut *core::ptr::addr_of_mut!(RXQ) };
        if q.tail == q.head {
            break;
        }
        let idx = q.tail;
        let len = q.len[idx] as usize;
        // SAFETY: `RXQ.data[idx]` is a live `len`-byte frame in the mailbox.
        let f = unsafe { core::slice::from_raw_parts(q.data[idx].as_ptr(), len) };
        dispatch(d, f);
        q.tail = (q.tail + 1) % RX_RING;
        n += 1;
    }
    n
}

fn hw_tx(d: &mut Dev, frame: &[u8], qid: u8) -> i32 {
    if d.hw.is_null() {
        return ENODEV;
    }
    // SAFETY: `d.hw` is live while registered.
    unsafe { hw::radio_tx(d.hw, frame, qid) }
}

// ── BSS list ──────────────────────────────────────────────────────────────────
fn find_bss<'a>(list: &'a [Bss], bssid: &[u8; 6]) -> Option<&'a Bss> {
    list.iter().find(|b| &b.bssid == bssid)
}

fn upsert_bss(d: &mut Dev, nb: &Bss) {
    for b in d.bss[..d.bss_count].iter_mut() {
        if &b.bssid == &nb.bssid {
            if nb.channel != 0 {
                b.channel = nb.channel;
            }
            b.privacy = nb.privacy;
            if nb.basic_rates != 0 {
                b.basic_rates = nb.basic_rates;
            }
            b.erp_flags = nb.erp_flags;
            if nb.supp_rates_len != 0 {
                b.supp_rates = nb.supp_rates;
                b.supp_rates_len = nb.supp_rates_len;
            }
            if b.ssid_len == 0 && nb.ssid_len > 0 {
                b.ssid = nb.ssid;
                b.ssid_len = nb.ssid_len;
            }
            if nb.rsn_len != 0 {
                b.rsn = nb.rsn;
                b.rsn_len = nb.rsn_len;
            }
            return;
        }
    }
    if d.bss_count >= MAX_BSS {
        return;
    }
    d.bss[d.bss_count] = *nb;
    if d.bss[d.bss_count].channel == 0 {
        d.bss[d.bss_count].channel = d.channel;
    }
    d.bss_count += 1;
}

// ── RX dispatch ───────────────────────────────────────────────────────────────
fn dispatch(d: &mut Dev, f: &[u8]) {
    let (ftype, fsub) = frame::frame_type_sub(f);
    if ftype == 0 {
        match fsub {
            8 | 5 => {
                if let Some(nb) = frame::parse_beacon(f, d.channel) {
                    if fsub == 8 && d.bssid != [0u8; 6] && nb.bssid == d.bssid {
                        d.beacon_rx = d.beacon_rx.wrapping_add(1);
                        if d.assoc {
                            d.last_beacon_us = now_us();
                        }
                    }
                    upsert_bss(d, &nb);
                }
            }
            11 => {
                if f.len() >= 30 {
                    d.auth_status = (f[28] as i32) | ((f[29] as i32) << 8);
                }
            }
            1 => {
                if f.len() >= 30 {
                    d.assoc_status = (f[26] as i32) | ((f[27] as i32) << 8);
                }
            }
            12 | 10 => {
                if f.len() >= 22 && d.bssid != [0u8; 6] && f[10..16] == d.bssid {
                    d.deauth_rx = d.deauth_rx.wrapping_add(1);
                    d.assoc = false;
                    d.secured = false;
                }
            }
            _ => {}
        }
        return;
    }
    if ftype != 2 {
        return;
    }
    if frame::is_eapol_rx(f) {
        rx_eapol(d, f);
    } else {
        rx_data(d, f);
    }
}

fn rx_eapol(d: &mut Dev, f: &[u8]) {
    if !d.assoc || f.len() < DOT11_HDR {
        return;
    }
    let hdr = frame::data_hdr_len(f);
    if f.len() < hdr + LLC_SNAP_LEN {
        return;
    }
    let payload = &f[hdr + LLC_SNAP_LEN..];
    if payload.len() > EAPOL_MAX {
        return;
    }
    let next = (d.eapol_head + 1) % EAPOL_Q;
    if next == d.eapol_tail {
        return; // full
    }
    d.eapol[d.eapol_head][..payload.len()].copy_from_slice(payload);
    d.eapol_src[d.eapol_head].copy_from_slice(&f[10..16]); // Addr2 = BSSID
    d.eapol_len[d.eapol_head] = payload.len() as u16;
    d.eapol_head = next;
}

fn rx_data(d: &mut Dev, f: &[u8]) {
    if !d.assoc || f.len() < DOT11_HDR {
        return;
    }
    if f[1] & 0x02 == 0 {
        return; // FromDS only
    }
    if f[10..16] != d.bssid {
        d.rx_other += 1;
        return;
    }
    let hdr = frame::data_hdr_len(f);
    if f.len() < hdr {
        return;
    }

    if f[1] & 0x40 != 0 {
        if f.len() < hdr + 16 {
            return;
        }
        let key_id = (f[hdr + 3] >> 6) & 0x3;
        let have = if key_id != 0 { d.have_gtk } else { d.have_tk };
        if !have {
            d.rx_nokey += 1;
            return;
        }
        let mut key = [0u8; 16];
        key.copy_from_slice(if key_id != 0 { &d.gtk } else { &d.tk });
        // SAFETY: `RX_SCRATCH` is the engine's decrypt buffer, used only here.
        let buf = unsafe { &mut *core::ptr::addr_of_mut!(RX_SCRATCH) };
        let n = f.len().min(buf.len());
        buf[..n].copy_from_slice(&f[..n]);
        match crate::ccmp::decrypt(&key, buf, n, hdr) {
            Some(plen) if plen >= LLC_SNAP_LEN => {
                deliver_ethernet(d, buf, hdr, plen);
            }
            _ => d.rx_bad += 1,
        }
    } else {
        deliver_ethernet(d, f, hdr, f.len() - hdr);
    }
}

/// Turn the LLC/SNAP payload at `buf[base..base+plen]` into an Ethernet frame.
fn deliver_ethernet(d: &mut Dev, buf: &[u8], base: usize, plen: usize) {
    if plen < LLC_SNAP_LEN {
        return;
    }
    let llc = &buf[base..base + plen];
    if !(llc[0] == 0xaa && llc[1] == 0xaa && llc[2] == 0x03) {
        return;
    }
    let ethertype = ((llc[6] as u16) << 8) | llc[7] as u16;
    let payload = &llc[LLC_SNAP_LEN..];
    if payload.is_empty() || payload.len() > 2048 - 14 {
        return;
    }
    let skb = skb_alloc();
    if skb.is_null() {
        return;
    }
    // SAFETY: `skb` is a fresh, non-null skb.
    let dst = unsafe { skb_put(skb, (14 + payload.len()) as u16) };
    if dst.is_null() {
        kfree_skb(skb);
        return;
    }
    // SAFETY: `dst` points at the freshly reserved (14 + payload.len()) bytes.
    let out = unsafe { core::slice::from_raw_parts_mut(dst, 14 + payload.len()) };
    out[..6].copy_from_slice(&buf[4..10]); // A1 = destination
    out[6..12].copy_from_slice(&buf[16..22]); // A3 = original source
    out[12] = (ethertype >> 8) as u8;
    out[13] = ethertype as u8;
    out[14..].copy_from_slice(payload);

    d.rx_data += 1;
    if buf[4] & 0x01 != 0 {
        d.rx_group += 1;
    }
    // SAFETY: `skb` is a complete Ethernet frame that `netif_rx` consumes.
    unsafe { netif_rx(skb) };
}

// ── netdev callbacks ──────────────────────────────────────────────────────────
extern "C" fn net_send(skb: *mut Skb) -> i32 {
    if skb.is_null() {
        return 0;
    }
    // SAFETY: `skb` is the live buffer the stack handed down for transmission.
    let (ptr, len) = unsafe { (skb_data(skb), skb_len(skb) as usize) };
    let mut rc = 0;
    if !ptr.is_null() && len >= 14 {
        // SAFETY: `ptr`/`len` describe the Ethernet frame in the skb.
        let eth = unsafe { core::slice::from_raw_parts(ptr, len) };
        radio_lock();
        rc = tx_ethernet(dev(), eth);
        radio_unlock();
    }
    // SAFETY: `skb` came from the stack's pool and is consumed here.
    kfree_skb(skb);
    rc
}

extern "C" fn net_poll() {
    radio_lock();
    let d = dev();
    pump(d);
    // Beacon-loss watchdog: drop the link when the AP goes quiet.  The
    // `beacon_rx` status counter tells whether beacons were arriving at all.
    if d.assoc && now_us().saturating_sub(d.last_beacon_us) > BEACON_LOSS_US {
        d.assoc = false;
        d.secured = false;
    }
    radio_unlock();
}

/// Build the 802.11 frame for an Ethernet frame the stack queued and hand it to
/// the radio.  Returns 0 when it reached the radio, or a negative errno when it
/// was dropped before that (not associated, or no key installed) — the drops are
/// counted in the status counters so a caller can tell "sent" from "dropped".
fn tx_ethernet(d: &mut Dev, eth: &[u8]) -> i32 {
    if !d.assoc || eth.len() < 14 {
        d.tx_drop_noassoc = d.tx_drop_noassoc.wrapping_add(1);
        return EAGAIN;
    }
    let mut dst = [0u8; 6];
    dst.copy_from_slice(&eth[..6]);
    let ethertype = ((eth[12] as u16) << 8) | eth[13] as u16;
    let payload = &eth[14..];
    let is_group = eth[0] & 0x01 != 0;

    let mut f = [0u8; 1600];
    let n = match frame::build_data_tods(&d.our_mac, &d.bssid, &dst, ethertype, payload, &mut f) {
        Some(n) => n,
        None => return EINVAL,
    };

    let rc = if d.privacy {
        // A group frame needs the GTK, a unicast one the TK: sending without
        // the right key would encrypt under an all-zero key and be dropped by
        // the AP, silently (a broadcast is never acknowledged).
        let have = if is_group { d.have_gtk } else { d.have_tk };
        if !d.secured || !have {
            d.tx_drop_nokey = d.tx_drop_nokey.wrapping_add(1);
            d.last_error = EAGAIN;
            return EAGAIN;
        }
        let (key, key_id) = if is_group { (d.gtk, d.gtk_id) } else { (d.tk, 0) };
        d.pn[key_id as usize & 3] += 1;
        let pn = d.pn[key_id as usize & 3];
        f[1] |= 0x40; // Protected
        let mut out = [0u8; 1600];
        match crate::ccmp::encrypt(&key, key_id, pn, &f[..DOT11_HDR], DOT11_HDR, &f[DOT11_HDR..n], &mut out) {
            Some(tot) => hw_tx(d, &out[..tot], Q_DATA),
            None => EINVAL,
        }
    } else {
        hw_tx(d, &f[..n], Q_DATA)
    };

    d.tx_data += 1;
    if is_group {
        d.tx_group += 1;
    }
    if rc != 0 {
        d.tx_fail += 1;
    }
    rc
}

// ── scan ──────────────────────────────────────────────────────────────────────
pub fn scan() -> i32 {
    radio_lock();
    let d = dev();
    d.bss_count = 0;

    let mut rc = 0;
    for ch in 1u8..=13 {
        d.channel = ch;
        apply_config(d, false);
        send_probe(d);

        let mut quiet = 0u32;
        let mut i = 0u32;
        while i < SCAN_POLLS_MAX && quiet < SCAN_QUIET {
            if i != 0 && i % SCAN_REPROBE == 0 {
                send_probe(d);
            }
            if pump(d) > 0 {
                quiet = 0;
            } else {
                quiet += 1;
            }
            i += 1;
        }
        if d.hw.is_null() {
            rc = ENODEV;
            break;
        }
    }
    radio_unlock();
    if rc != 0 {
        rc
    } else {
        d.bss_count as i32
    }
}

fn send_probe(d: &mut Dev) {
    let m = d.our_mac;
    let f = frame::build_probe_request(&m);
    let _ = hw_tx(d, &f, hw::Q_MGMT);
}

// ── connect (open-system auth + association) ──────────────────────────────────
pub fn connect(bssid: &[u8; 6], ssid: &[u8]) -> i32 {
    radio_lock();
    let rc = connect_inner(bssid, ssid);
    radio_unlock();
    rc
}

fn connect_inner(bssid: &[u8; 6], ssid: &[u8]) -> i32 {
    let d = dev();
    let bss = match find_bss(&d.bss[..d.bss_count], bssid) {
        Some(b) => *b,
        None => return ENOENT,
    };

    d.bssid = bss.bssid;
    d.privacy = bss.privacy != 0;
    d.channel = if bss.channel != 0 { bss.channel } else { d.channel };
    d.assoc = false;
    d.secured = false;
    d.have_tk = false;
    d.have_gtk = false;
    d.eapol_head = 0;
    d.eapol_tail = 0;

    apply_config(d, false);
    sleep_us(CTS_SETTLE_US);

    // Open-system authentication.
    d.auth_status = -1;
    let auth = frame::build_auth(&d.our_mac, bssid);
    let _ = hw_tx(d, &auth, hw::Q_MGMT);
    if !wait_status(d, Status::Auth) {
        return ETIMEDOUT;
    }
    if d.auth_status != 0 {
        return -200 - d.auth_status; // auth rejected; status in the low bits
    }

    // Association.  `_ssid` is implicit in the parsed BSS.
    let _ = ssid;
    d.assoc_status = -1;
    let mut f = [0u8; 256];
    let n = match frame::build_assoc(&d.our_mac, &bss, &mut f) {
        Some(n) => n,
        None => return EINVAL,
    };
    let _ = hw_tx(d, &f[..n], hw::Q_MGMT);
    if !wait_status(d, Status::Assoc) {
        return ETIMEDOUT;
    }
    if d.assoc_status != 0 {
        return -300 - d.assoc_status;
    }

    d.assoc = true;
    d.last_beacon_us = now_us();
    apply_config(d, true);
    0
}

enum Status {
    Auth,
    Assoc,
}

fn wait_status(d: &mut Dev, which: Status) -> bool {
    let deadline = now_us() + MGMT_TIMEOUT_US;
    loop {
        pump(d);
        let done = match which {
            Status::Auth => d.auth_status >= 0,
            Status::Assoc => d.assoc_status >= 0,
        };
        if done {
            return true;
        }
        if now_us() >= deadline || d.hw.is_null() {
            return false;
        }
    }
}

// ── EAPOL (the userspace supplicant's transport) ──────────────────────────────
pub fn eapol_tx(dst: &[u8; 6], payload: &[u8]) -> i32 {
    radio_lock();
    let d = dev();
    let rc = if !d.assoc {
        EIO
    } else {
        let mut f = [0u8; 1600];
        match frame::build_data_tods(&d.our_mac, &d.bssid, dst, 0x888e, payload, &mut f) {
            Some(n) => hw_tx(d, &f[..n], Q_DATA),
            None => EINVAL,
        }
    };
    radio_unlock();
    rc
}

pub fn eapol_rx(out: &mut [u8]) -> Option<(usize, [u8; 6])> {
    radio_lock();
    let d = dev();
    // Poll once so a pending EAPOL frame can arrive; then drain the queue.
    pump(d);
    let res = if d.eapol_tail != d.eapol_head {
        let idx = d.eapol_tail;
        let len = d.eapol_len[idx] as usize;
        let src = d.eapol_src[idx];
        let n = len.min(out.len());
        out[..n].copy_from_slice(&d.eapol[idx][..n]);
        d.eapol_tail = (d.eapol_tail + 1) % EAPOL_Q;
        Some((n, src))
    } else {
        None
    };
    radio_unlock();
    res
}

// ── keys / status ─────────────────────────────────────────────────────────────
pub fn set_key(kind: u32, key_id: u32, key: &[u8]) -> i32 {
    if key.is_empty() || key.len() > 16 {
        return EINVAL;
    }
    radio_lock();
    let d = dev();
    if kind == 1 {
        // Group key.
        d.gtk[..key.len()].copy_from_slice(key);
        d.gtk_id = (key_id & 0x3) as u8;
        d.have_gtk = true;
    } else {
        d.tk[..key.len()].copy_from_slice(key);
        d.have_tk = true;
    }
    if d.have_tk {
        d.secured = true;
    }
    radio_unlock();
    0
}

pub fn disconnect() -> i32 {
    radio_lock();
    let d = dev();
    d.assoc = false;
    d.secured = false;
    d.have_tk = false;
    d.have_gtk = false;
    radio_unlock();
    0
}

/// Fill the `cact_wlan_status_t` fields (see `control.rs`).
pub struct StatusView {
    pub linked: i32,
    pub mac: [u8; 6],
    pub bssid: [u8; 6],
    pub channel: u32,
    pub ssid: [u8; 33],
    pub last_error: i32,
    pub tx_data: u32,
    pub tx_group: u32,
    pub tx_fail: u32,
    pub tx_drop_noassoc: u32,
    pub tx_drop_nokey: u32,
    pub rx_data: u32,
    pub rx_group: u32,
    pub rx_bad: u32,
    pub rx_nokey: u32,
    pub rx_other: u32,
    pub beacon_rx: u32,
    pub deauth_rx: u32,
}

pub fn status() -> StatusView {
    radio_lock();
    let d = dev();
    let mut ssid = [0u8; 33];
    if let Some(b) = find_bss(&d.bss[..d.bss_count], &d.bssid) {
        let n = b.ssid_len as usize;
        ssid[..n].copy_from_slice(&b.ssid[..n]);
    }
    let view = StatusView {
        linked: if d.assoc { 1 } else { 0 },
        mac: d.our_mac,
        bssid: d.bssid,
        channel: d.channel as u32,
        ssid,
        last_error: d.last_error,
        tx_data: d.tx_data,
        tx_group: d.tx_group,
        tx_fail: d.tx_fail,
        tx_drop_noassoc: d.tx_drop_noassoc,
        tx_drop_nokey: d.tx_drop_nokey,
        rx_data: d.rx_data,
        rx_group: d.rx_group,
        rx_bad: d.rx_bad,
        rx_nokey: d.rx_nokey,
        rx_other: d.rx_other,
        beacon_rx: d.beacon_rx,
        deauth_rx: d.deauth_rx,
    };
    radio_unlock();
    view
}

/// Copy BSS `index` into `out`; returns 0 or a negative errno.
pub fn bss_get(index: u32, out: &mut BssEntry) -> i32 {
    radio_lock();
    let d = dev();
    if index as usize >= d.bss_count {
        radio_unlock();
        return ENOENT;
    }
    let b = &d.bss[index as usize];
    out.bssid = b.bssid;
    out.ssid = [0; 33];
    out.ssid[..b.ssid_len as usize].copy_from_slice(&b.ssid[..b.ssid_len as usize]);
    out.ssid_len = b.ssid_len;
    out.channel = b.channel;
    out.rssi = 0;
    out.privacy = b.privacy;
    out.rsn_len = b.rsn_len;
    out.rsn[..b.rsn_len as usize].copy_from_slice(&b.rsn[..b.rsn_len as usize]);
    radio_unlock();
    0
}

pub struct BssEntry {
    pub bssid: [u8; 6],
    pub ssid: [u8; 33],
    pub ssid_len: u8,
    pub channel: u8,
    pub rssi: i8,
    pub privacy: u8,
    pub rsn_len: u8,
    pub rsn: [u8; 64],
}

/// Serve the AP list as text for `read(/dev/wlan0)`.
pub fn bss_list_text(out: &mut [u8]) -> usize {
    radio_lock();
    let d = dev();
    let mut n = 0usize;
    for b in d.bss[..d.bss_count].iter() {
        if n + 80 >= out.len() {
            break;
        }
        let line = format_bss(b);
        let bytes = line.as_bytes();
        out[n..n + bytes.len()].copy_from_slice(bytes);
        n += bytes.len();
    }
    radio_unlock();
    n
}

fn hex2(v: u8, out: &mut [u8]) {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    out[0] = HEX[(v >> 4) as usize];
    out[1] = HEX[(v & 0xf) as usize];
}

fn format_bss(b: &Bss) -> LineBuffer {
    let mut buf = LineBuffer::new();
    for (i, &byte) in b.bssid.iter().enumerate() {
        if i != 0 {
            buf.push(b':');
        }
        let mut h = [0u8; 2];
        hex2(byte, &mut h);
        buf.push(h[0]);
        buf.push(h[1]);
    }
    buf.push_str(b" ch=");
    buf.push_u32(b.channel as u32);
    buf.push(b' ');
    buf.push_str(if b.privacy != 0 { b"WPA2 " } else { b"open " });
    let n = b.ssid_len as usize;
    for &c in &b.ssid[..n] {
        if c == 0 {
            break;
        }
        buf.push(c);
    }
    buf.push(b'\n');
    buf
}

/// A tiny fixed-capacity line buffer (no `alloc`).
struct LineBuffer {
    buf: [u8; 96],
    len: usize,
}

impl LineBuffer {
    fn new() -> Self {
        LineBuffer { buf: [0; 96], len: 0 }
    }
    fn push(&mut self, c: u8) {
        if self.len < self.buf.len() {
            self.buf[self.len] = c;
            self.len += 1;
        }
    }
    fn push_str(&mut self, s: &[u8]) {
        for &c in s {
            self.push(c);
        }
    }
    fn push_u32(&mut self, mut v: u32) {
        let mut digits = [0u8; 10];
        let mut used = 0;
        loop {
            digits[used] = b'0' + (v % 10) as u8;
            used += 1;
            v /= 10;
            if v == 0 {
                break;
            }
        }
        while used > 0 {
            used -= 1;
            self.push(digits[used]);
        }
    }
    fn as_bytes(&self) -> &[u8] {
        &self.buf[..self.len]
    }
}

// ── engine entry: driver RX callback lands here ───────────────────────────────
pub fn rx_frame(_hw: *mut Cact80211Hw, f: &[u8]) {
    rx_enqueue(f);
}
