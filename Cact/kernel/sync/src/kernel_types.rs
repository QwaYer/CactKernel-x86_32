use core::ffi::c_void;

#[repr(C)]
pub struct ProcPageTracker {
    pub pages:    *mut *mut c_void,
    pub count:    u32,
    pub capacity: u32,
    pub page_dir: *mut u32,
}

#[repr(C)]
pub struct MmapTable {
    _opaque: [u8; 7172],
}

#[repr(C)]
pub struct VfsNode {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct TaskFdTable {
    pub fd_table:   [*mut VfsNode; 256],
    pub fd_offset:  [u32; 256],
    pub fd_flags:   [u32; 256],
    pub fd_cloexec: [u32; 256],
}

#[repr(C)]
pub struct DynCtx {
    _opaque: [u8; 0],
}
