//! `/dev/wlan0`: the engine's control surface for the userspace supplicant.
//!
//! Linux-shaped split: the kernel runs scan / auth / assoc / the datapath / CCMP,
//! and `wljoin` is the WPA2 supplicant — it reads the BSS list, asks the kernel
//! to join a BSS, exchanges EAPOL with the AP, and installs the keys.  `read()`
//! still serves the AP list as human-readable text.

use core::ffi::{c_char, c_int, c_void};

use crate::dev;
use crate::ffi::{self, DevfsDriver, DEVFS_F_CHAR};

// ── ABI (mirrors the CACT_WLANCTL_* block in ioctl_abi.h) ─────────────────────
const WLANCTL_SCAN: u32 = 0x3F00;
const WLANCTL_GET_BSS: u32 = 0x3F01;
const WLANCTL_CONNECT: u32 = 0x3F02;
const WLANCTL_DISCONNECT: u32 = 0x3F03;
const WLANCTL_SET_KEY: u32 = 0x3F05;
const WLANCTL_STATUS: u32 = 0x3F06;
const WLANCTL_EAPOL_TX: u32 = 0x3F07;
const WLANCTL_EAPOL_RX: u32 = 0x3F08;

const SSID_MAX: usize = 33;
const RSN_MAX: usize = 64;
const KEY_MAX: usize = 32;
const EAPOL_MAX: usize = 512;
const EFAULT: i32 = -14;
const EINVAL: i32 = -22;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CactWlanBss {
    pub index: u32,
    pub bssid: [u8; 6],
    pub ssid: [u8; SSID_MAX],
    pub ssid_len: u8,
    pub channel: u8,
    pub rssi: i8,
    pub privacy: u8,
    pub rsn_len: u8,
    pub rsn: [u8; RSN_MAX],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CactWlanConnect {
    pub bssid: [u8; 6],
    pub ssid: [u8; SSID_MAX],
    pub ssid_len: u8,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CactWlanKey {
    pub kind: u32,
    pub key_id: u32,
    pub key_len: u32,
    pub key: [u8; KEY_MAX],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CactWlanEapol {
    pub dst: [u8; 6],
    pub len: u32,
    pub data: [u8; EAPOL_MAX],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CactWlanStatus {
    pub linked: i32,
    pub mac: [u8; 6],
    pub bssid: [u8; 6],
    pub channel: u32,
    pub ssid: [u8; SSID_MAX],
    pub last_error: i32,
    pub tx_data: u32,
    pub tx_group: u32,
    pub tx_fail: u32,
    pub rx_data: u32,
    pub rx_group: u32,
    pub rx_bad: u32,
    pub rx_nokey: u32,
    pub rx_other: u32,
    pub tx_drop_noassoc: u32,
    pub tx_drop_nokey: u32,
    pub beacon_rx: u32,
    pub deauth_rx: u32,
}

const _: () = assert!(core::mem::size_of::<CactWlanBss>() == 112);
const _: () = assert!(core::mem::offset_of!(CactWlanBss, ssid) == 10);
const _: () = assert!(core::mem::offset_of!(CactWlanBss, rsn) == 48);
const _: () = assert!(core::mem::size_of::<CactWlanConnect>() == 40);
const _: () = assert!(core::mem::size_of::<CactWlanKey>() == 44);
const _: () = assert!(core::mem::size_of::<CactWlanEapol>() == 524);
const _: () = assert!(core::mem::offset_of!(CactWlanEapol, len) == 8);
const _: () = assert!(core::mem::offset_of!(CactWlanEapol, data) == 12);
const _: () = assert!(core::mem::size_of::<CactWlanStatus>() == 108);

// ── user copies ───────────────────────────────────────────────────────────────
fn copy_in<T: Copy>(arg: *mut c_void, out: &mut T) -> bool {
    if arg.is_null() {
        return false;
    }
    let size = core::mem::size_of::<T>() as u32;
    // `validate_user_ptr` returns non-zero when the range is a valid user range.
    // SAFETY: a kernel validation service; it only inspects the pointer.
    if unsafe { ffi::validate_user_ptr(arg, size) } == 0 {
        return false;
    }
    // SAFETY: `out` is a live `T` and `arg` was just validated for `size` bytes.
    unsafe { ffi::copy_from_user(out as *mut T as *mut c_void, arg, size) == 0 }
}

fn copy_out<T>(arg: *mut c_void, val: &T) -> bool {
    if arg.is_null() {
        return false;
    }
    let size = core::mem::size_of::<T>() as u32;
    // SAFETY: a kernel validation service; it only inspects the pointer.
    if unsafe { ffi::validate_user_ptr(arg, size) } == 0 {
        return false;
    }
    // SAFETY: `arg` was just validated for `size` bytes and `val` is a live `T`.
    unsafe { ffi::copy_to_user(arg, val as *const T as *const c_void, size) == 0 }
}

// ── read(): the AP list as text ───────────────────────────────────────────────
static mut READ_BUF: [u8; 4096] = [0; 4096];

extern "C" fn wlan_read(_priv: *mut c_void, off: u32, size: u32, buf: *mut c_char) -> c_int {
    if buf.is_null() || size == 0 {
        return 0;
    }
    // SAFETY: `READ_BUF` is the engine's single read scratch buffer; devfs is
    // single-consumer per device.
    let text = unsafe { &mut *core::ptr::addr_of_mut!(READ_BUF) };
    let len = dev::bss_list_text(text);
    if off >= len as u32 {
        return 0;
    }
    let n = core::cmp::min(len as u32 - off, size) as usize;
    // The devfs read path hands a kernel-accessible buffer, so a direct copy is
    // correct (the retired C driver did the same).
    // SAFETY: `buf` is a `size`-byte writable buffer and `n <= size`.
    let dst = unsafe { core::slice::from_raw_parts_mut(buf as *mut u8, n) };
    dst.copy_from_slice(&text[off as usize..off as usize + n]);
    n as c_int
}

// ── ioctl() ───────────────────────────────────────────────────────────────────
extern "C" fn wlan_ioctl(_priv: *mut c_void, cmd: u32, arg: *mut c_void) -> c_int {
    match cmd {
        WLANCTL_SCAN => dev::scan(),
        WLANCTL_GET_BSS => do_get_bss(arg),
        WLANCTL_CONNECT => do_connect(arg),
        WLANCTL_DISCONNECT => dev::disconnect(),
        WLANCTL_SET_KEY => do_set_key(arg),
        WLANCTL_STATUS => do_status(arg),
        WLANCTL_EAPOL_TX => do_eapol_tx(arg),
        WLANCTL_EAPOL_RX => do_eapol_rx(arg),
        _ => EINVAL,
    }
}

fn do_get_bss(arg: *mut c_void) -> c_int {
    let mut req = CactWlanBss {
        index: 0,
        bssid: [0; 6],
        ssid: [0; SSID_MAX],
        ssid_len: 0,
        channel: 0,
        rssi: 0,
        privacy: 0,
        rsn_len: 0,
        rsn: [0; RSN_MAX],
    };
    if !copy_in(arg, &mut req) {
        return EFAULT;
    }
    let mut entry = dev::BssEntry {
        bssid: [0; 6],
        ssid: [0; SSID_MAX],
        ssid_len: 0,
        channel: 0,
        rssi: 0,
        privacy: 0,
        rsn_len: 0,
        rsn: [0; RSN_MAX],
    };
    let rc = dev::bss_get(req.index, &mut entry);
    if rc != 0 {
        return rc;
    }
    req = CactWlanBss {
        index: req.index,
        bssid: entry.bssid,
        ssid: entry.ssid,
        ssid_len: entry.ssid_len,
        channel: entry.channel,
        rssi: entry.rssi,
        privacy: entry.privacy,
        rsn_len: entry.rsn_len,
        rsn: entry.rsn,
    };
    if copy_out(arg, &req) {
        0
    } else {
        EFAULT
    }
}

fn do_connect(arg: *mut c_void) -> c_int {
    let mut req = CactWlanConnect {
        bssid: [0; 6],
        ssid: [0; SSID_MAX],
        ssid_len: 0,
    };
    if !copy_in(arg, &mut req) {
        return EFAULT;
    }
    let n = core::cmp::min(req.ssid_len as usize, SSID_MAX);
    dev::connect(&req.bssid, &req.ssid[..n])
}

fn do_set_key(arg: *mut c_void) -> c_int {
    let mut req = CactWlanKey {
        kind: 0,
        key_id: 0,
        key_len: 0,
        key: [0; KEY_MAX],
    };
    if !copy_in(arg, &mut req) {
        return EFAULT;
    }
    let n = core::cmp::min(req.key_len as usize, KEY_MAX);
    if n == 0 {
        return EINVAL;
    }
    dev::set_key(req.kind, req.key_id, &req.key[..n])
}

fn do_status(arg: *mut c_void) -> c_int {
    let s = dev::status();
    let st = CactWlanStatus {
        linked: s.linked,
        mac: s.mac,
        bssid: s.bssid,
        channel: s.channel,
        ssid: s.ssid,
        last_error: s.last_error,
        tx_data: s.tx_data,
        tx_group: s.tx_group,
        tx_fail: s.tx_fail,
        rx_data: s.rx_data,
        rx_group: s.rx_group,
        rx_bad: s.rx_bad,
        rx_nokey: s.rx_nokey,
        rx_other: s.rx_other,
        tx_drop_noassoc: s.tx_drop_noassoc,
        tx_drop_nokey: s.tx_drop_nokey,
        beacon_rx: s.beacon_rx,
        deauth_rx: s.deauth_rx,
    };
    if copy_out(arg, &st) {
        0
    } else {
        EFAULT
    }
}

fn do_eapol_tx(arg: *mut c_void) -> c_int {
    let mut req = CactWlanEapol {
        dst: [0; 6],
        len: 0,
        data: [0; EAPOL_MAX],
    };
    if !copy_in(arg, &mut req) {
        return EFAULT;
    }
    let n = core::cmp::min(req.len as usize, EAPOL_MAX);
    if n == 0 {
        return EINVAL;
    }
    dev::eapol_tx(&req.dst, &req.data[..n])
}

fn do_eapol_rx(arg: *mut c_void) -> c_int {
    let mut req = CactWlanEapol {
        dst: [0; 6],
        len: 0,
        data: [0; EAPOL_MAX],
    };
    if !copy_in(arg, &mut req) {
        return EFAULT;
    }
    let mut buf = [0u8; EAPOL_MAX];
    match dev::eapol_rx(&mut buf) {
        Some((n, src)) => {
            req.dst = src;
            req.len = n as u32;
            req.data[..n].copy_from_slice(&buf[..n]);
        }
        None => {
            req.len = 0;
        }
    }
    if copy_out(arg, &req) {
        0
    } else {
        EFAULT
    }
}

// ── devfs registration ────────────────────────────────────────────────────────
static mut DRIVER: DevfsDriver = DevfsDriver {
    read: Some(wlan_read),
    write: None,
    ctl: None,
    status: None,
    ioctl: Some(wlan_ioctl),
    walk: None,
    readdir: None,
};

static mut NODE_UP: bool = false;

pub fn register_node() {
    // SAFETY: `NODE_UP` is this engine's one-time flag.
    if unsafe { NODE_UP } {
        return;
    }
    // SAFETY: `DRIVER` is a kernel-lifetime static table; `register_chrdev`
    // records it.  `wlan0` is registered once per radio.
    let ok = unsafe {
        !ffi::register_chrdev(
            b"wlan0\0".as_ptr() as *const c_char,
            DEVFS_F_CHAR,
            core::ptr::addr_of_mut!(DRIVER),
            core::ptr::null_mut(),
        )
        .is_null()
    };
    if ok {
        // SAFETY: publishing the one-time flag.
        unsafe { NODE_UP = true };
    }
}

pub fn unregister_node() {
    // SAFETY: `NODE_UP` is the engine's flag.
    if !unsafe { NODE_UP } {
        return;
    }
    // SAFETY: `unregister_chrdev` takes a NUL-terminated name.
    unsafe { ffi::unregister_chrdev(b"wlan0\0".as_ptr() as *const c_char) };
    // SAFETY: clearing the flag.
    unsafe { NODE_UP = false };
}
