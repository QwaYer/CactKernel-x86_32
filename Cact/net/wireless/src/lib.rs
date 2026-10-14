#![no_std]
#![deny(unsafe_op_in_unsafe_fn)]
#![deny(unused_unsafe)]
#![deny(static_mut_refs)]
#![deny(improper_ctypes, improper_ctypes_definitions)]

//! In-kernel 802.11 station engine — the mac80211-equivalent for CactKernel.
//!
//! The Wi-Fi driver is a dumb radio: it brings up the chip, sets the channel,
//! and moves raw 802.11 MPDUs.  This engine owns the 802.11 protocol — scan,
//! open-system auth/association, the 802.3↔802.11 datapath and CCMP — and
//! registers `wlan0` as the NIC.  The WPA2 4-way handshake stays in userspace
//! (`wljoin`), which drives the engine through `/dev/wlan0` (`CACT_WLANCTL_*`).
//!
//! A radio registers through [`hw::Cact80211Hw`] (see `cact80211.h`); the driver
//! keeps its own `#[no_mangle]` module entry points and calls
//! `cact80211_register` / `cact80211_rx` (exported through `ksym`).

mod aes;
mod ccmp;
mod control;
mod dev;
mod ffi;
mod frame;
mod hw;
