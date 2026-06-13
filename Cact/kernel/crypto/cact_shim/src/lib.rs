#![no_std]
#![allow(non_camel_case_types)]

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
