//! Layout-fixed C types shared with the kernel FFI layer.
//!
//! This module is the *single* Rust definition of these C structures:
//! `cact_mm` re-exports `MmapTable` / `MmapRegion` / `ProcPageTracker` from
//! here instead of keeping copies, so a pointer to one of them has the same
//! Rust type in `sched`, `cact_mm` and anywhere else.  (The copies had already
//! drifted once — see `rust_net/src/types.rs`' `Semaphore` for the story.)

/// Number of regions in an [`MmapTable`] (mirrors `MMAP_MAX_REGIONS` in C).
pub const MMAP_MAX_REGIONS: usize = 256;

/// Tracks user pages owned by a process (C `ProcPageTracker`).
#[repr(C)]
pub struct ProcPageTracker {
    pub pages:    *mut *mut u8,
    pub count:    u32,
    pub capacity: u32,
    pub page_dir: *mut u32,
}

/// One `mmap` region (C `mmap_region_t`).
#[repr(C)]
pub struct MmapRegion {
    pub base:     u32,
    pub length:   u32,
    pub flags:    u32,
    pub prot:     u32,
    pub fd:       i32,
    pub file_off: u32,
    pub is_used:  u8,
    /// Handle of the shared backing object (memfd) for `MAP_SHARED` mappings,
    /// or 0 for plain private/anon/file mappings.
    pub shobj:    i32,
}

/// Per-process memory mapping metadata (C `mmap_table_t`).
#[repr(C)]
pub struct MmapTable {
    pub regions:   [MmapRegion; MMAP_MAX_REGIONS],
    pub next_base: u32,
}

/// ABI check: the C `mmap_table_t` is 8196 bytes (the Rust side used to declare
/// this struct as an opaque `[u8; 8196]`; the real layout must match it exactly).
const _: () = assert!(core::mem::size_of::<MmapTable>() == 8196);

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
