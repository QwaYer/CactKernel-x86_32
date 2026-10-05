//! Pure physical-memory math: page/address conversion, bitmap indexing, region
//! clipping and the RAM/MMIO boundary computation.
//!
//! No hardware access, no globals and no FFI, so this module is the single
//! source of truth for the PMM's arithmetic and is unit-tested on the host by
//! `Kernel-Unit-Tests-for-Cact/` (P2.1).  `pmm.rs` calls into it; nothing here
//! may reference `crate::`.

/// Physical page size (4 KiB).
pub const PAGE_SIZE: u32 = 4096;

/// Page index -> physical address.
#[inline]
pub const fn page_to_addr(idx: u32) -> u32 {
    idx * PAGE_SIZE
}

/// Physical address -> page index (floor).
#[inline]
pub const fn addr_to_page(addr: u32) -> u32 {
    addr / PAGE_SIZE
}

/// Byte offset of `idx` inside the free-frame bitmap.
#[inline]
pub const fn bitmap_byte(idx: u32) -> usize {
    (idx / 8) as usize
}

/// Bit mask of `idx` inside its bitmap byte.
#[inline]
pub const fn bitmap_mask(idx: u32) -> u8 {
    1u8 << (idx % 8)
}

/// Clip a physical region `[base, base+len)` to `[.., limit)` (the 4 GiB
/// physical ceiling).  Returns the clipped `(base, end)`, or `None` when the
/// region has no bytes below `limit`.
pub fn clip_to_limit(base: u64, len: u64, limit: u64) -> Option<(u64, u64)> {
    if len == 0 {
        return None;
    }
    let end = base.saturating_add(len).min(limit);
    if base >= end {
        return None;
    }
    Some((base, end))
}

/// Half-open page range `[first, last)` covering `[base, end)`.  `first` is
/// rounded up to a page boundary, `last` down; callers must skip the region
/// when `first >= last`.  Mirrors the original PMM install loop exactly,
/// including the 32-bit truncation of the masked addresses.
pub fn aligned_page_range(base: u64, end: u64) -> (u32, u32) {
    let mask = PAGE_SIZE as u64 - 1;
    let first_addr = (base + mask) & !mask;
    let last_addr = end & !mask;
    (addr_to_page(first_addr as u32), addr_to_page(last_addr as u32))
}

/// Page-aligned top of available RAM from the boot map's highest usable end
/// address.  Returns `None` when there is nothing usable (caller keeps its
/// default boundary), `Some(re)` otherwise, with `re` clamped below `limit` and
/// never below `reserved_end`.
pub fn ram_end_from_candidate(candidate: u64, reserved_end: u32, limit: u64) -> Option<u32> {
    if candidate == 0 {
        return None;
    }
    let mask = PAGE_SIZE as u64 - 1;
    let mut re = (candidate + mask) & !mask;
    if re >= limit {
        re = limit - PAGE_SIZE as u64;
    }
    if re >= reserved_end as u64 {
        Some(re as u32)
    } else {
        None
    }
}

/// Base VA of the fixed kernel MMIO windows (ACPI temp map, PCIe ECAM): the
/// greater of the RAM top and the PCI hole, rounded up to 2 MiB so a window
/// never aliases a managed RAM frame under the identity map.
pub const fn mmio_window_base(ram_end: u32, hole_start: u32) -> u32 {
    const ALIGN: u32 = 2 * 1024 * 1024;
    let base = if ram_end > hole_start { ram_end } else { hole_start };
    (base + (ALIGN - 1)) & !(ALIGN - 1)
}
