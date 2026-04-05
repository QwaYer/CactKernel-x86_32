#![no_std]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(non_upper_case_globals)]
#![allow(clippy::missing_safety_doc)]
pub mod ffi;
pub mod sync;
pub mod task;
pub mod mlfq;
pub mod timer_wheel;

#[panic_handler]
fn panic_handler(_info: &core::panic::PanicInfo) -> ! {
    unsafe {
        core::arch::asm!("cli");
        loop {
            core::arch::asm!("hlt");
        }
    }
}

const _ABI_CHECK: () = {
    use core::mem::offset_of;
    use task::TaskStruct;

    assert!(offset_of!(TaskStruct, esp)            ==  0, "esp offset mismatch");
    assert!(offset_of!(TaskStruct, pid)            ==  4, "pid offset mismatch");
    assert!(offset_of!(TaskStruct, state)          ==  8, "state offset mismatch");
    assert!(offset_of!(TaskStruct, is_kernel)      == 12, "is_kernel offset mismatch");
    assert!(offset_of!(TaskStruct, _pad0)          == 13, "_pad0 offset mismatch");
    assert!(offset_of!(TaskStruct, stack_base)     == 16, "stack_base offset mismatch");
    assert!(offset_of!(TaskStruct, ustack_phys)    == 20, "ustack_phys offset mismatch");
    assert!(offset_of!(TaskStruct, ustack_virt)    == 24, "ustack_virt offset mismatch");
    assert!(offset_of!(TaskStruct, page_directory) == 28, "page_directory offset mismatch");
    assert!(offset_of!(TaskStruct, next)           == 32, "next offset mismatch");
    assert!(offset_of!(TaskStruct, queue_next)     == 36, "queue_next offset mismatch");
};
