#![no_std]
#![allow(static_mut_refs)]

//! In-kernel TCP/IP stack (smoltcp): Ethernet shim, sockets, DHCP/DNS helpers, and
//! integration hooks for the C networking layer.

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
