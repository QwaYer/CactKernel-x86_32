//! Opaque and layout-fixed C types shared with the kernel FFI layer.

use core::ffi::c_void;

/// Tracks user pages owned by a process (C `ProcPageTracker`).
#[repr(C)]
pub struct ProcPageTracker {
    pub pages:    *mut *mut c_void,
    pub count:    u32,
    pub capacity: u32,
    pub page_dir: *mut u32,
}

/// Per-process memory mapping metadata; size matches the C struct (opaque bytes).
#[repr(C)]
pub struct MmapTable {
    _opaque: [u8; 7172],
}

/// VFS inode pointer as seen from Rust (unsized in C headers; zero-sized placeholder).
#[repr(C)]
pub struct VfsNode {
    _opaque: [u8; 0],
}

/// Open file table: pointers into VFS plus per-fd metadata.
#[repr(C)]
pub struct TaskFdTable {
    pub fd_table:   [*mut VfsNode; 256],
    pub fd_offset:  [u32; 256],
    pub fd_flags:   [u32; 256],
    pub fd_cloexec: [u32; 256],
}
