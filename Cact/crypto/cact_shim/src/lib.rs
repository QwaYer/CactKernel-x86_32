#![no_std]
#![allow(non_camel_case_types)]
// Safety baseline: unsafe ops inside `unsafe fn` bodies must be explicit, and every
// `unsafe` block needs a SAFETY comment (the latter enforced under clippy).
#![deny(unsafe_op_in_unsafe_fn)]
#![deny(unused_unsafe)]
#![deny(clippy::undocumented_unsafe_blocks)]
#![deny(clippy::missing_safety_doc)]
#![deny(static_mut_refs)]
#![deny(improper_ctypes, improper_ctypes_definitions)]
// Additional safety lints (all verified zero-hit when enabled, 2026-09-26):
// transmute misuse, byte-vs-element count confusion and assumptions that
// uninitialised memory is valid become hard errors instead of silent warnings.
#![deny(clippy::transmute_ptr_to_ref)]
#![deny(clippy::transmute_ptr_to_ptr)]
#![deny(clippy::useless_transmute)]
#![deny(clippy::size_of_in_element_count)]
#![deny(clippy::uninit_assumed_init)]
#![deny(invalid_reference_casting)]
// Maximal-safety: one unsafe operation per `unsafe` block, so each `// SAFETY:`
// justifies exactly one operation.
#![deny(clippy::multiple_unsafe_ops_per_block)]

extern crate alloc;

pub mod error;
pub mod io;
pub mod time;
pub mod sync;
pub mod net;
pub mod marker;
pub mod convert;
pub mod ops;
pub mod fmt;
pub mod mem;
pub mod thread;
pub mod vec;
pub mod boxed;
pub mod string;
pub mod collections;
pub mod hint;

#[macro_export]
macro_rules! println {
    () => { $crate::io::_print("\n") };
    ($($arg:tt)*) => {{
        $crate::io::_print(alloc::format!($($arg)*).as_str());
    }};
}

#[macro_export]
macro_rules! print {
    ($($arg:tt)*) => {{
        $crate::io::_print(alloc::format!($($arg)*).as_str());
    }};
}

pub mod prelude {
    pub mod v1 {
        pub use crate::io::{Read, Write, BufRead};
    }
}
