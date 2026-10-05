//! Pure VFS helpers: the dcache bucket index and the fixed `child -> parent`
//! map used to resolve `..` for dirfd-relative lookups.
//!
//! No hardware access, no globals and no FFI, so this is the single source of
//! truth for that arithmetic and is unit-tested on the host by
//! `Kernel-Unit-Tests-for-Cact/` (P2.1).  `dcache.rs` owns the storage and the
//! lock and calls in here.

/// Number of dcache buckets (must match `dcache::DCACHE_BUCKETS`).
pub const DCACHE_BUCKETS: usize = 128;
/// Fixed capacity of the parent map (must match `dcache::PARENT_MAX`).
pub const PARENT_MAX: usize = 256;

/// Bucket index for `(parent_key, name_hash)`, matching the original
/// `(parent ^ hash) & (BUCKETS - 1)`.
pub const fn bucket_index(parent_key: u32, name_hash: u32) -> usize {
    ((parent_key ^ name_hash) as usize) & (DCACHE_BUCKETS - 1)
}

/// Parent recorded for `child`, or `None`.  `count` bounds the live prefix of
/// `pairs`; it is clamped to the slice length for safety.
pub fn parent_lookup(pairs: &[(usize, usize)], count: usize, child: usize) -> Option<usize> {
    let n = count.min(pairs.len());
    for e in &pairs[..n] {
        if e.0 == child {
            return Some(e.1);
        }
    }
    None
}

/// Record `child -> parent`: overwrite an existing entry, else append while
/// there is room.  Returns `true` only when a new entry was appended.
pub fn parent_insert(
    pairs: &mut [(usize, usize)],
    count: &mut usize,
    child: usize,
    parent: usize,
) -> bool {
    let n = (*count).min(pairs.len());
    for e in pairs[..n].iter_mut() {
        if e.0 == child {
            e.1 = parent;
            return false;
        }
    }
    if *count < pairs.len() {
        pairs[*count] = (child, parent);
        *count += 1;
        return true;
    }
    false
}
