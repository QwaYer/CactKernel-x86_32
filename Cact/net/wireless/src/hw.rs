//! The driver-facing ("dumb radio") API: the ops a Wi-Fi driver registers with
//! the in-kernel 802.11 engine, plus the engine's entry points.
//!
//! Layer split (decided 2026-10-10, Linux-shaped): the driver is the radio —
//! firmware, MAC/BBP/RF init, channel, raw 802.11 TX/RX.  This engine owns the
//! 802.11 protocol: scan, auth/assoc, the 802.3↔802.11 datapath and CCMP.  The
//! WPA2 4-way handshake stays in userspace (`wljoin`).  The `cact80211.h`
//! header shipped to modules mirrors these layouts byte for byte.

use core::ffi::{c_char, c_int, c_void};

/// TX queue class the engine hands to the radio; the driver maps it to a
/// bulk-OUT endpoint.  (Not the hardware's queue id — that stays in the driver.)
pub const Q_MGMT: u8 = 0;
pub const Q_DATA: u8 = 1;

/// Radio configuration, mirroring `struct cact80211_conf` in `cact80211.h`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Cact80211Conf {
    pub channel: u8,
    pub assoc: u8,
    pub bssid: [u8; 6],
    pub basic_rates: u16,
    pub erp_flags: u16,
}

#[repr(C)]
pub struct Cact80211Ops {
    pub tx: Option<extern "C" fn(*mut c_void, *const u8, c_int, u8) -> c_int>,
    pub config: Option<extern "C" fn(*mut c_void, *const Cact80211Conf)>,
    pub rx_poll: Option<extern "C" fn(*mut c_void) -> c_int>,
}

/// Mirrors `struct cact80211_hw` in `cact80211.h`.
#[repr(C)]
pub struct Cact80211Hw {
    pub ops: *const Cact80211Ops,
    pub priv_: *mut c_void,
    pub mac: [u8; 6],
    pub name: *const c_char,
}

const _: () = assert!(core::mem::size_of::<Cact80211Conf>() == 12);
const _: () = assert!(core::mem::offset_of!(Cact80211Conf, bssid) == 2);
const _: () = assert!(core::mem::offset_of!(Cact80211Conf, basic_rates) == 8);
const _: () = assert!(core::mem::size_of::<Cact80211Ops>() == 12);
const _: () = assert!(core::mem::size_of::<Cact80211Hw>() == 20);
const _: () = assert!(core::mem::offset_of!(Cact80211Hw, mac) == 8);
const _: () = assert!(core::mem::offset_of!(Cact80211Hw, name) == 16);

/// Transmit one raw 802.11 MPDU (no FCS) on the given queue class.
///
/// # Safety
/// `hw` must be a live `Cact80211Hw` whose `ops` table is valid.
pub unsafe fn radio_tx(hw: *mut Cact80211Hw, frame: &[u8], qid: u8) -> c_int {
    if hw.is_null() {
        return -22;
    }
    // SAFETY: the caller contract makes `hw` valid for the read.
    let ops = unsafe { (*hw).ops };
    if ops.is_null() || frame.len() > 2312 {
        return -22;
    }
    // SAFETY: `ops` was checked non-null; `tx` is an optional function pointer.
    match unsafe { (*ops).tx } {
        // SAFETY: the driver's `tx` contract is (priv, buf, len, qid).
        Some(f) => unsafe { f((*hw).priv_, frame.as_ptr(), frame.len() as c_int, qid) },
        None => -22,
    }
}

/// Program the radio (channel, BSSID/association, basic rates, ERP).
///
/// # Safety
/// `hw` must be a live `Cact80211Hw` whose `ops` table is valid.
pub unsafe fn radio_config(hw: *mut Cact80211Hw, conf: &Cact80211Conf) {
    if hw.is_null() {
        return;
    }
    // SAFETY: the caller contract makes `hw` valid for the read.
    let ops = unsafe { (*hw).ops };
    if ops.is_null() {
        return;
    }
    // SAFETY: `ops` was checked non-null; `config` is an optional pointer.
    if let Some(f) = unsafe { (*ops).config } {
        // SAFETY: the driver's `config` contract is (priv, conf).
        unsafe { f((*hw).priv_, conf) };
    }
}

/// Drive one receive poll on the radio; the driver calls `cact80211_rx` for
/// each frame it delivers.  Returns the number of frames (or <= 0).
///
/// # Safety
/// `hw` must be a live `Cact80211Hw` whose `ops` table is valid.
pub unsafe fn radio_rx_poll(hw: *mut Cact80211Hw) -> c_int {
    if hw.is_null() {
        return 0;
    }
    // SAFETY: the caller contract makes `hw` valid for the read.
    let ops = unsafe { (*hw).ops };
    if ops.is_null() {
        return 0;
    }
    // SAFETY: `ops` was checked non-null; `rx_poll` is an optional pointer.
    match unsafe { (*ops).rx_poll } {
        // SAFETY: the driver's `rx_poll` contract is (priv).
        Some(f) => unsafe { f((*hw).priv_) },
        None => 0,
    }
}

// ── Engine entry points (exported to modules through ksym) ────────────────────

/// Register a radio with the engine: brings up `wlan0` (netdev + control node).
/// Returns 0 on success, a negative errno otherwise.
#[no_mangle]
pub extern "C" fn cact80211_register(hw: *mut Cact80211Hw) -> c_int {
    crate::dev::register(hw)
}

/// Tear a radio down (netdev + control node + state).
#[no_mangle]
pub extern "C" fn cact80211_unregister(hw: *mut Cact80211Hw) {
    crate::dev::unregister(hw);
}

/// Hand one received 802.11 MPDU to the engine (called by the driver's RX path).
///
/// # Safety
/// `frame` must point to `len` readable bytes; both live only for the call.
#[no_mangle]
pub unsafe extern "C" fn cact80211_rx(hw: *mut Cact80211Hw, frame: *const u8, len: c_int) {
    if hw.is_null() || frame.is_null() || len <= 0 || len > 2312 {
        return;
    }
    // SAFETY: the caller contract makes `frame` a `len`-byte readable buffer.
    let f = unsafe { core::slice::from_raw_parts(frame, len as usize) };
    crate::dev::rx_frame(hw, f);
}
