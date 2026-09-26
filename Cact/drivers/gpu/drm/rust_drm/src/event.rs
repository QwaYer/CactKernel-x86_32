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
    // SAFETY: `crtc` is a live CRTC owned by the caller; crtc_init zeroes the
    // whole struct, so `mode` is initialised and can be read by value here.
    unsafe {
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
    // SAFETY: `crtc` is the caller's live CRTC (checked non-null above).
    let period = unsafe { frame_period_usec(crtc) };
    // SAFETY: `ktime_get_usec` is a kernel C service reading the monotonic clock.
    let now = unsafe { ktime_get_usec() } as u32;
    // SAFETY: `crtc` is the caller's live CRTC; this borrow is consumed by the
    // counter updates below, and neither call above is in its live range.
    let crtc = unsafe { &mut *crtc };
    let last = crtc.vblank_last_usec;
    if last == 0 {
        crtc.vblank_last_usec = now;
        return;
    }
    let elapsed = now.wrapping_sub(last);
    if elapsed < period {
        return;
    }
    let n = elapsed / period;
    crtc.vblank_count = crtc.vblank_count.wrapping_add(n);
    crtc.vblank_last_usec = last.wrapping_add(n * period);
}

/// Advance a CRTC's sequence counter.
#[no_mangle]
pub extern "C" fn drm_crtc_vblank_bump(crtc: *mut Crtc) {
    if crtc.is_null() {
        return;
    }
    // SAFETY: `crtc` is the caller's live CRTC (checked non-null above); this
    // borrow is consumed by the single counter update.
    let crtc = unsafe { &mut *crtc };
    crtc.vblank_count += 1;
}

/// A real vblank interrupt arrived: bump the counter and wake every client.
/// The event carries the new sequence so libdrm can match it with the flip it
/// queued.
#[no_mangle]
pub extern "C" fn drm_crtc_handle_vblank(crtc: *mut Crtc) {
    if crtc.is_null() {
        return;
    }
    // SAFETY: `crtc` is the caller's live CRTC (checked non-null above), so this
    // field read is in bounds.
    let dev = unsafe { (*crtc).dev };
    if dev.is_null() {
        return;
    }
    drm_crtc_vblank_bump(crtc);
    // SAFETY: the counter is re-read after the bump, as the original did, so the
    // event carries the new sequence number.
    let seq = unsafe { (*crtc).vblank_count };
    drm_file_queue_event(dev, crtc, DRM_EVENT_VBLANK, 0, seq);
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
    // SAFETY: `dev` is the caller's live device; this borrow of its client list is
    // consumed by the walk below (under the caller's lock).
    let clients = unsafe { &(*dev).clients };
    // SAFETY: `crtc` is the caller's live CRTC; this reads its id.
    let crtc_id = unsafe { (*crtc).id };
    for &f in clients.iter() {
        if f.is_null() {
            continue;
        }
        // SAFETY: `ktime_get_usec` is a kernel C service reading the monotonic clock.
        let tv_usec = unsafe { ktime_get_usec() } as u32;
        let ev = EventVblank {
            base: crate::structs::Event {
                etype: event_type,
                length: core::mem::size_of::<EventVblank>() as u32,
            },
            user_data,
            tv_sec: 0,
            tv_usec,
            sequence: seq,
            crtc_id,
        };
        // SAFETY: `f` is a live client (non-null, checked above); queueing the event
        // on it.
        unsafe { (*f).events.push_back(ev) };
    }
}
