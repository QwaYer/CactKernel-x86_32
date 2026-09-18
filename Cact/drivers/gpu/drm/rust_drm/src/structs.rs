//! The core's own state, and the `#[repr(C)]` mirrors of what a driver sees.
//!
//! Two kinds of type live here:
//!
//!   * **Driver-visible** (`drm_drv.h`): `DriverOps`, `GemObject`, `ModeSet`, a
//!     handful of uapi structs.  Their layout is the module ABI and is pinned
//!     with size/offset assertions, exactly as before.
//!   * **Core-private**: `DrmDevice`, `DrmFile`, properties and blobs.  These
//!     used to be C structs mirrored field-for-field with fixed-capacity pools;
//!     now that no C reads them, they are ordinary Rust types whose collections
//!     come from the kernel allocator — a device may hold any number of CRTCs,
//!     framebuffers, properties or map offsets, and a client any number of
//!     handles and events.

use alloc::collections::{BTreeMap, VecDeque};
use alloc::vec::Vec;
use core::ffi::{c_int, c_void};

use crate::mode::DisplayMode;
use crate::vfs::VfsNode;

/* ── name lengths (fixed-size strings, not pools) ───────────────────────── */

pub const FILE_NAME_MAX: usize = 64;
pub const PROPERTY_NAME_LEN: usize = 32;

/* ── sync primitives (kernel/sync/sync.h) ───────────────────────────────── */

/// `spinlock_t` — opaque here: the C `irq_spinlock_*` helpers are the only
/// things that touch it.  Size pinned so it can be embedded.
#[repr(C)]
pub struct Spinlock {
    _opaque: [u8; 4],
}

/// `irq_spinlock_t` — likewise opaque.
#[repr(C)]
pub struct IrqSpinlock {
    _opaque: [u8; 8],
}

impl IrqSpinlock {
    /// An uninitialised lock; `irq_spinlock_init` must be called before use.
    pub const fn new() -> Self {
        Self { _opaque: [0; 8] }
    }
}

const _: () = assert!(core::mem::size_of::<Spinlock>() == 4);
const _: () = assert!(core::mem::size_of::<IrqSpinlock>() == 8);

/* ── GEM (drm_drv.h) ────────────────────────────────────────────────────── */

/// `struct drm_gem_object` — driver-visible: a driver receives one from
/// `gem_create`/`gem_free` and can ask the core for its size and storage.
#[repr(C)]
pub struct GemObject {
    pub dev: *mut DrmDevice,
    pub size: u32,
    pub refcount: i32,
    /// memfd handle backing the object's frames.
    pub memfd: i32,
    pub width: u32,
    pub height: u32,
    pub bpp: u32,
    pub pitch: u32,
    pub is_dumb: i32,
    pub map_offset: u32,
}

const _: () = assert!(core::mem::size_of::<GemObject>() == 40);
const _: () = assert!(core::mem::offset_of!(GemObject, memfd) == 12);
const _: () = assert!(core::mem::offset_of!(GemObject, is_dumb) == 32);
const _: () = assert!(core::mem::offset_of!(GemObject, map_offset) == 36);

/* ── events (uapi/drm.h) ────────────────────────────────────────────────── */

/// `struct drm_event` (uapi/drm.h).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Event {
    pub etype: u32,
    pub length: u32,
}

/// `struct drm_event_vblank` (uapi/drm.h).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct EventVblank {
    pub base: Event,
    pub user_data: u64,
    pub tv_sec: u32,
    pub tv_usec: u32,
    pub sequence: u32,
    pub crtc_id: u32,
}

const _: () = assert!(core::mem::size_of::<EventVblank>() == 32);

/* ── properties / blobs (core-private) ──────────────────────────────────── */

#[allow(dead_code)] // ptype/val_len describe the property for a future ABI
pub struct Property {
    pub id: u32,
    pub name: [u8; PROPERTY_NAME_LEN],
    pub flags: u32,
    pub ptype: u32,
    pub values: Vec<u64>,
    pub enum_names: Vec<*const u8>,
    pub val_len: u32,
}

pub struct PropAttach {
    pub prop_id: u32,
    pub obj_type: u32,
    pub obj_id: u32,
    pub value: u64,
}

/// A blob property's payload: immutable bytes a client reads back by id.
pub struct Blob {
    pub id: u32,
    pub data: Vec<u8>,
}

/// A syncobj: something a client can wait on and signal.
///
/// The binary flavour has a single "signalled" bit; the timeline flavour has a
/// point that only moves forward.  Which one an object is fixed at creation, as
/// in Linux, so a binary wait on a timeline object is an error and vice versa.
pub struct Syncobj {
    pub id: u32,
    pub timeline: bool,
    pub signaled: bool,
    pub point: u64,
}

/* ── the driver-ops table (drm_drv.h) ───────────────────────────────────── */

#[repr(C)]
pub struct DriverOps {
    pub name: *const u8,
    pub desc: *const u8,
    pub major: u32,
    pub minor: u32,
    pub patchlevel: u32,
    pub driver_date: u32,

    pub load: Option<extern "C" fn(*mut DrmDevice) -> c_int>,
    pub unload: Option<extern "C" fn(*mut DrmDevice)>,
    pub mode_valid: Option<extern "C" fn(*mut c_void, *const DisplayMode) -> c_int>,
    pub set_config: Option<extern "C" fn(*mut DrmDevice, *mut ModeSet) -> c_int>,
    pub page_flip: Option<extern "C" fn(*mut c_void, *mut c_void, u32, *mut c_void) -> c_int>,
    pub dirty: Option<extern "C" fn(*mut c_void, *const c_void, u32) -> c_int>,
    pub enable_vblank: Option<extern "C" fn(*mut DrmDevice, *mut c_void) -> c_int>,
    pub disable_vblank: Option<extern "C" fn(*mut DrmDevice, *mut c_void)>,
    pub gem_create: Option<extern "C" fn(*mut DrmDevice, *mut GemObject) -> c_int>,
    pub gem_free: Option<extern "C" fn(*mut GemObject)>,
    pub close: Option<extern "C" fn(*mut DrmDevice, *mut DrmFile)>,
    pub ioctl: Option<extern "C" fn(*mut DrmDevice, *mut DrmFile, u32, *mut c_void, u32) -> c_int>,
    pub cursor_set:
        Option<extern "C" fn(*mut c_void, *mut GemObject, u32, u32, i32, i32) -> c_int>,
    pub cursor_move: Option<extern "C" fn(*mut c_void, i32, i32) -> c_int>,
}

const _: () = assert!(core::mem::size_of::<DriverOps>() == 80);
const _: () = assert!(core::mem::offset_of!(DriverOps, load) == 24);
const _: () = assert!(core::mem::offset_of!(DriverOps, set_config) == 36);
const _: () = assert!(core::mem::offset_of!(DriverOps, gem_create) == 56);
const _: () = assert!(core::mem::offset_of!(DriverOps, ioctl) == 68);
const _: () = assert!(core::mem::offset_of!(DriverOps, cursor_set) == 72);
const _: () = assert!(core::mem::offset_of!(DriverOps, cursor_move) == 76);

/* ── the non-atomic modeset request (drm_drv.h) ─────────────────────────── */

/// `struct drm_mode_set`.  The connector set is a pointer into the core's own
/// storage, so the list is not capped; a driver only ever reads `crtc`/`fb`.
#[repr(C)]
pub struct ModeSet {
    pub fb: *mut c_void,
    pub crtc: *mut c_void,
    pub mode: DisplayMode,
    pub x: i32,
    pub y: i32,
    pub connectors: *mut *mut c_void,
    pub num_connectors: i32,
}

const _: () = assert!(core::mem::offset_of!(ModeSet, crtc) == 4);
const _: () = assert!(core::mem::offset_of!(ModeSet, mode) == 8);
const _: () = assert!(core::mem::offset_of!(ModeSet, connectors) == 80);
const _: () = assert!(core::mem::offset_of!(ModeSet, num_connectors) == 84);

/* ── file ───────────────────────────────────────────────────────────────── */

/// `struct drm_file` — per-open client state.  Core-private; a driver only ever
/// holds the pointer the core hands it.  `repr(C)` only so the extern
/// signatures that carry a `*mut DrmFile` are FFI-safe.
#[repr(C)]
pub struct DrmFile {
    pub dev: *mut DrmDevice,
    pub is_render: i32,
    pub is_master: i32,
    pub authenticated: i32,
    pub magic: u32,
    #[allow(dead_code)]
    pub name: [u8; FILE_NAME_MAX],

    /// Per-client GEM handles.  `next_handle` only moves forward, so a handle is
    /// never reused while the client lives.
    pub handles: BTreeMap<u32, *mut GemObject>,
    pub next_handle: u32,

    /// Framebuffers this client created.
    pub fbs: Vec<u32>,

    /// Syncobj handles this client holds, mapped to the object's device id.
    pub syncobjs: BTreeMap<u32, u32>,
    pub next_syncobj_handle: u32,

    /// Queued vblank / page-flip events, drained by read(2).
    pub events: VecDeque<EventVblank>,
}

/* ── device ─────────────────────────────────────────────────────────────── */

/// `struct drm_device` — per-device core state.  Core-private.
///
/// Every pool is allocator-backed; `crtcs[i]` and friends are indexed by
/// `id - 1`, and ids only move forward, so an entry is never reused under a
/// stale id.  `repr(C)` only so the extern signatures that carry a
/// `*mut DrmDevice` are FFI-safe.
#[repr(C)]
pub struct DrmDevice {
    pub ops: *const DriverOps,
    pub priv_: *mut c_void,
    pub minor: i32,
    pub in_use: i32,
    pub next_magic: u32,
    pub next_fb_id: u32,
    pub next_prop_id: u32,
    #[allow(dead_code)]
    pub next_blob_id: u32,

    /* Guards the device's core bookkeeping.  Never held while calling into the
     * driver: a driver may sleep on a semaphore waiting for its hardware. */
    pub lock: IrqSpinlock,

    pub crtcs: Vec<*mut c_void>,
    pub connectors: Vec<*mut c_void>,
    pub encoders: Vec<*mut c_void>,
    pub planes: Vec<*mut c_void>,
    pub fbs: Vec<*mut c_void>,

    pub props: Vec<Property>,
    pub prop_attach: Vec<PropAttach>,
    pub blobs: Vec<Blob>,

    /// Faked mmap offsets handed out by DRM_IOCTL_MODE_MAP_DUMB.
    pub map_offsets: BTreeMap<u32, *mut GemObject>,
    pub next_map_offset: u32,

    /// Every live GEM object of this device (PRIME import looks objects up by
    /// their memfd handle here).
    pub gem_list: Vec<*mut GemObject>,

    /// Global GEM names (DRM_IOCTL_GEM_FLINK / GEM_OPEN).
    pub flink: BTreeMap<u32, *mut GemObject>,
    pub next_flink_name: u32,

    pub clients: Vec<*mut DrmFile>,

    /// Syncobjs this device owns, and the sync-file fds exported from them
    /// (`fd`, object id, the node the fd refers to).  The node is recorded so
    /// closing the fd can drop the entry.
    pub syncobjs: Vec<Syncobj>,
    pub next_syncobj_id: u32,
    pub sync_fds: Vec<(i32, u32, *mut VfsNode)>,

    /* The VFS nodes published under /dev/dri. */
    pub card_node: *mut VfsNode,
    pub render_node: *mut VfsNode,
    pub have_render_node: i32,
}

/// Place `p` in `pool[index]`, growing the pool as needed.  Ids stay
/// `index + 1`, so a slot is only ever empty because it was freed or skipped.
/// The caller checks the slot is free first.
pub fn pool_set(pool: &mut Vec<*mut c_void>, index: usize, p: *mut c_void) -> u32 {
    while pool.len() <= index {
        pool.push(core::ptr::null_mut());
    }
    pool[index] = p;
    (index + 1) as u32
}

impl DrmDevice {
    /// Look a CRTC/connector/encoder/plane up by the id userspace uses.  The
    /// pools are id-indexed, so this is a checked index.
    fn find_in(pool: &[*mut c_void], id: u32) -> *mut c_void {
        if id == 0 {
            return core::ptr::null_mut();
        }
        match pool.get((id - 1) as usize) {
            Some(&p) => p,
            None => core::ptr::null_mut(),
        }
    }

    pub fn find_crtc(&self, id: u32) -> *mut c_void {
        Self::find_in(&self.crtcs, id)
    }
    pub fn find_connector(&self, id: u32) -> *mut c_void {
        Self::find_in(&self.connectors, id)
    }
    pub fn find_encoder(&self, id: u32) -> *mut c_void {
        Self::find_in(&self.encoders, id)
    }
    pub fn find_plane(&self, id: u32) -> *mut c_void {
        Self::find_in(&self.planes, id)
    }
    pub fn find_fb(&self, id: u32) -> *mut c_void {
        if id == 0 {
            return core::ptr::null_mut();
        }
        for &p in &self.fbs {
            if !p.is_null() && unsafe { (*(p as *const crate::kms::framebuffer::Framebuffer)).id } == id {
                return p;
            }
        }
        core::ptr::null_mut()
    }

    pub fn find_prop(&self, id: u32) -> *mut Property {
        if id == 0 {
            return core::ptr::null_mut();
        }
        for p in &self.props {
            if p.id == id {
                return p as *const Property as *mut Property;
            }
        }
        core::ptr::null_mut()
    }

    /// Look a device property up by name.  Used for the properties the core
    /// defines itself (a connector's `EDID` blob), which must exist once per
    /// device and be shared by every object that attaches it.
    pub fn find_prop_by_name(&self, name: &[u8]) -> u32 {
        for p in &self.props {
            let end = p.name.iter().position(|&b| b == 0).unwrap_or(p.name.len());
            if &p.name[..end] == name {
                return p.id;
            }
        }
        0
    }

    /// Look a property up by name *and* object type.  Linux gives the same name
    /// different property objects per object class — a connector's `CRTC_ID` is
    /// not a plane's `CRTC_ID` — and the atomic ioctl uses the type to know
    /// which object pool an id refers to.
    pub fn find_prop_by_name_type(&self, name: &[u8], ptype: u32) -> u32 {
        for p in &self.props {
            if p.ptype != ptype {
                continue;
            }
            let end = p.name.iter().position(|&b| b == 0).unwrap_or(p.name.len());
            if &p.name[..end] == name {
                return p.id;
            }
        }
        0
    }

    pub fn find_blob(&self, id: u32) -> *mut Blob {
        if id == 0 {
            return core::ptr::null_mut();
        }
        for b in &self.blobs {
            if b.id == id {
                return b as *const Blob as *mut Blob;
            }
        }
        core::ptr::null_mut()
    }

    /// Store `data` as a new blob and return its id.  Ids only move forward, so
    /// a destroyed blob's id is never handed out again.
    pub fn blob_create(&mut self, data: &[u8]) -> u32 {
        self.next_blob_id += 1;
        let id = self.next_blob_id;
        self.blobs.push(Blob {
            id,
            data: Vec::from(data),
        });
        id
    }

    pub fn blob_destroy(&mut self, id: u32) -> bool {
        match self.blobs.iter().position(|b| b.id == id) {
            Some(i) => {
                self.blobs.remove(i);
                true
            }
            None => false,
        }
    }

    pub fn find_syncobj(&self, id: u32) -> *mut Syncobj {
        if id == 0 {
            return core::ptr::null_mut();
        }
        for s in &self.syncobjs {
            if s.id == id {
                return s as *const Syncobj as *mut Syncobj;
            }
        }
        core::ptr::null_mut()
    }

    /// Create a syncobj and return its device id.  Ids only move forward.
    pub fn syncobj_create(&mut self, timeline: bool, signaled: bool, point: u64) -> u32 {
        self.next_syncobj_id += 1;
        let id = self.next_syncobj_id;
        self.syncobjs.push(Syncobj {
            id,
            timeline,
            signaled,
            point,
        });
        id
    }

    pub fn syncobj_destroy(&mut self, id: u32) -> bool {
        match self.syncobjs.iter().position(|s| s.id == id) {
            Some(i) => {
                self.syncobjs.remove(i);
                true
            }
            None => false,
        }
    }
}
