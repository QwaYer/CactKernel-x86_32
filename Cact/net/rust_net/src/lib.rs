#![no_std]
#![feature(sync_unsafe_cell)]
#![allow(static_mut_refs)]

//! In-kernel TCP/IP stack (smoltcp): Ethernet shim, sockets, DHCP/DNS helpers, and
//! integration hooks for the C networking layer.

extern crate alloc;

use core::alloc::{GlobalAlloc, Layout};

struct CactAllocator;

unsafe impl GlobalAlloc for CactAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        unsafe extern "C" { fn kmalloc_aligned(size: usize, align: u32) -> *mut core::ffi::c_void; }
        kmalloc_aligned(layout.size(), layout.align() as u32) as *mut u8
    }
    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        unsafe extern "C" { fn kfree_heap(ptr: *mut core::ffi::c_void); }
        kfree_heap(ptr as *mut core::ffi::c_void);
    }
}

#[global_allocator]
static ALLOCATOR: CactAllocator = CactAllocator;

pub mod checksum;
pub mod config;
pub mod dns_resolve;
pub mod dhcp;
pub mod ffi;
pub mod ffi_kernel;
pub mod ping;
pub mod runtime;
pub mod socket;
pub mod stack;
pub mod skb;
pub mod tcp;
pub mod tls;
pub mod types;
pub mod udp;

#[panic_handler]
fn panic_handler(_info: &core::panic::PanicInfo) -> ! {
    unsafe {
        core::arch::asm!("cli");
        loop {
            core::arch::asm!("hlt");
        }
    }
}
