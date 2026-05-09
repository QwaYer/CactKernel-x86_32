use core::ffi::c_void;

pub use cact_sync::kernel_types::{
    DynCtx, MmapTable, ProcPageTracker, TaskFdTable, VfsNode,
};

#[repr(C)]
pub struct ContextFrame {
    pub es:       u32,
    pub ds:       u32,
    pub edi:      u32,
    pub esi:      u32,
    pub ebp:      u32,
    pub esp_dummy:u32,
    pub ebx:      u32,
    pub edx:      u32,
    pub ecx:      u32,
    pub eax:      u32,
    pub int_no:   u32,
    pub err_code: u32,
    pub eip:      u32,
    pub cs:       u32,
    pub eflags:   u32,
    pub useresp:  u32,
    pub ss:       u32,
}

extern "C" {
    pub fn kmalloc(size: usize) -> *mut c_void;
    pub fn kfree_heap(ptr: *mut c_void);
    pub fn kalloc() -> *mut c_void;       
    pub fn kfree_page(ptr: *mut c_void);

    pub fn memory_copy(dst: *mut c_void, src: *const c_void, size: usize);
    pub fn memory_set(dst: *mut c_void, val: u8, size: usize);

    pub fn vmm_create_address_space() -> *mut u32;
    pub fn vmm_free_address_space(pd: *mut u32);
    pub fn vmm_fork_address_space(src_pd: *mut u32, dst_pd: *mut u32);
    pub fn vmm_map(pd: *mut u32, virt: u32, phys: u32, flags: u32);
    pub fn vmm_sync_kernel_mmio_mappings(pd: *mut u32);

    pub fn load_elf(
        path:    *const u8,
        pd:      *mut u32,
        tracker: *mut ProcPageTracker,
    ) -> *mut c_void;
    pub fn load_elf_dynamic(
        path:    *const u8,
        pd:      *mut u32,
        tracker: *mut ProcPageTracker,
        ctx:     *mut DynCtx,
    ) -> *mut c_void;
    pub fn elf_is_dynamic(path: *const u8) -> i32;

    pub fn elf_get_brk_start(node: *mut VfsNode) -> u32;

    pub fn vfs_walk_path(root: *mut VfsNode, path: *const u8) -> *mut VfsNode;
    pub fn close_vfs(node: *mut VfsNode);
    pub fn open_vfs(node: *mut VfsNode);

    pub fn proc_tracker_add(tracker: *mut ProcPageTracker, phys: *mut c_void) -> i32;
    pub fn proc_free_pages(tracker: *mut ProcPageTracker);

    pub fn mmap_table_init(table: *mut MmapTable);
    pub fn mmap_table_clone(
        src: *mut MmapTable,
        dst: *mut MmapTable,
        src_pd: *mut u32,
        dst_pd: *mut u32,
    );
    pub fn mmap_table_free(table: *mut MmapTable, pd: *mut u32);

    pub fn shm_detach_all(pid: u32, pd: *mut u32);

    pub fn dynlink_unload_all(ctx: *mut DynCtx);
    pub fn dynlink_ctx_create(pd: *mut u32, tracker: *mut ProcPageTracker) -> *mut DynCtx;
    pub fn dynlink_ctx_destroy(ctx: *mut DynCtx);

    pub fn switch_to(old_esp: *mut u32, new_esp: u32);
    pub fn switch_paging(pd: *mut u32);

    pub fn kernel_task_trampoline();
    pub fn user_task_trampoline();
    pub fn fork_task_trampoline();

    pub fn kprint(s: *const u8);
    pub fn kprint_color(s: *const u8, color: u32);
    pub fn itoa(n: i32, buf: *mut u8);
    pub fn hex_to_ascii(n: u32, buf: *mut u8);
    pub fn klog(level: i32, s: *const u8);

    pub static mut page_directory: u32;       
    pub static mut tss_entry: TssEntry;
    pub static mut vfs_root: *mut VfsNode;
    pub static mut terminal_fg_pid: u32;
    pub static sys_sigreturn_num: u32;
}

#[repr(C)]
pub struct TssEntry {
    pub prev_tss: u32,
    pub esp0:     u32,   
    pub ss0:      u32,
}

pub const PAGE_PRESENT: u32 = 1 << 0;
pub const PAGE_RW:      u32 = 1 << 1;
pub const PAGE_USER:    u32 = 1 << 2;
pub const PAGE_SIZE:    u32 = 4096;

pub const LOG_OK:   i32 = 0;
pub const LOG_FAIL: i32 = 1;

pub const KERNEL_BASE: u32 = 0xC000_0000;

#[macro_export]
macro_rules! kprint {
    ($s:literal) => {
        unsafe { $crate::ffi::kprint(concat!($s, "\0").as_ptr()) }
    };
}

#[inline(always)]
pub unsafe fn proc_tracker_init(t: *mut ProcPageTracker) {
    (*t).pages    = core::ptr::null_mut();
    (*t).count    = 0;
    (*t).capacity = 0;
    (*t).page_dir = core::ptr::null_mut();
}

#[inline(always)]
pub unsafe fn cli() {
    core::arch::asm!("cli", options(nomem, nostack, preserves_flags));
}

#[inline(always)]
pub unsafe fn sti() {
    core::arch::asm!("sti", options(nomem, nostack, preserves_flags));
}

#[inline(always)]
pub unsafe fn read_eflags() -> u32 {
    let flags: u32;
    core::arch::asm!("pushfd; pop {}", out(reg) flags, options(nomem, nostack));
    flags
}

#[inline(always)]
pub unsafe fn pause() {
    core::arch::asm!("pause", options(nomem, nostack, preserves_flags));
}