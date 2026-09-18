//! Vblank accounting and the per-client event queue that `read()` on a card fd
//! drains in the `drm_event_vblank` form libdrm expects.
//!
//! Each client's queue is allocator-backed, so a slow reader accumulates as
//! many events as it has not yet drained instead of being capped.

use crate::kms::mode_object::Crtc;
use crate::structs::{DrmDevice, EventVblank};

/* uapi/drm.h */
const DRM_EVENT_VBLANK: u32 = 0x01;

extern "C" {
    fn ktime_get_usec() -> u64;
}

/// The CRTC's frame period in microseconds, from its mode's timings
/// (`clock` is in kHz, so one frame is `htotal * vtotal` pixels).
///
/// SAFETY: `crtc` is the caller's.
unsafe fn frame_period_usec(crtc: *mut Crtc) -> u32 {
    let m = (*crtc).mode;
    if m.clock != 0 && m.htotal != 0 && m.vtotal != 0 {
        let pixels = m.htotal as u64 * m.vtotal as u64;
        let hz = (m.clock as u64 * 1000) / pixels;
        if hz != 0 && hz <= 1000 {
            return (1_000_000 / hz) as u32;
        }
    }
    16_666 // no usable mode yet: assume 60 Hz
}

/// Bring a CRTC's vblank counter up to the current time.
///
/// virtio-gpu has no vblank interrupt (and no way to ask the device for a
/// period), so the core models vblanks from the wall clock and the mode's
/// refresh rate.  A client that asks for the current sequence therefore gets a
/// number that keeps moving on its own, which is what frame throttling and
/// `WAIT_VBLANK` need; a driver that *does* have an interrupt uses
/// `drm_crtc_handle_vblank()` instead and this never runs.
#[no_mangle]
pub extern "C" fn drm_crtc_vblank_advance(crtc: *mut Crtc) {
    if crtc.is_null() {
        return;
    }
    // SAFETY: caller's CRTC.
    unsafe {
        let period = frame_period_usec(crtc);
        let now = ktime_get_usec() as u32;
        let last = (*crtc).vblank_last_usec;
        if last == 0 {
            (*crtc).vblank_last_usec = now;
            return;
        }
        let elapsed = now.wrapping_sub(last);
        if elapsed < period {
            return;
        }
        let n = elapsed / period;
        (*crtc).vblank_count = (*crtc).vblank_count.wrapping_add(n);
        (*crtc).vblank_last_usec = last.wrapping_add(n * period);
    }
}

/// Advance a CRTC's sequence counter.
#[no_mangle]
pub extern "C" fn drm_crtc_vblank_bump(crtc: *mut Crtc) {
    if crtc.is_null() {
        return;
    }
    // SAFETY: caller's CRTC.
    unsafe {
        (*crtc).vblank_count += 1;
    }
}

/// A real vblank interrupt arrived: bump the counter and wake every client.
/// The event carries the new sequence so libdrm can match it with the flip it
/// queued.
#[no_mangle]
pub extern "C" fn drm_crtc_handle_vblank(crtc: *mut Crtc) {
    if crtc.is_null() {
        return;
    }
    // SAFETY: caller's CRTC; `dev` is checked before use.
    unsafe {
        if (*crtc).dev.is_null() {
            return;
        }
        drm_crtc_vblank_bump(crtc);
        drm_file_queue_event((*crtc).dev, crtc, DRM_EVENT_VBLANK, 0, (*crtc).vblank_count);
    }
}

/// Queue an event for every client of `dev`.
#[no_mangle]
pub extern "C" fn drm_file_queue_event(
    dev: *mut DrmDevice,
    crtc: *mut Crtc,
    event_type: u32,
    user_data: u64,
    seq: u32,
) {
    if dev.is_null() || crtc.is_null() {
        return;
    }
    // SAFETY: caller's device; the client list is walked under the caller's
    // lock (the same contract the C version had).
    unsafe {
        for &f in (*dev).clients.iter() {
            if f.is_null() {
                continue;
            }
            let ev = EventVblank {
                base: crate::structs::Event {
                    etype: event_type,
                    length: core::mem::size_of::<EventVblank>() as u32,
                },
                user_data,
                tv_sec: 0,
                tv_usec: ktime_get_usec() as u32,
                sequence: seq,
                crtc_id: (*crtc).id,
            };
            (*f).events.push_back(ev);
        }
    }
}
