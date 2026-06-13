use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;
use core::fmt;

pub trait Error: fmt::Display + fmt::Debug {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        None
    }
    fn description(&self) -> &str {
        "description() is deprecated; use Display"
    }
    fn cause(&self) -> Option<&dyn Error> {
        self.source()
    }
}

impl<'a, T: Error + Sized + 'a> From<T> for Box<dyn Error + 'a> {
    fn from(err: T) -> Box<dyn Error + 'a> {
        Box::new(err)
    }
}

impl<'a> From<&str> for Box<dyn Error + 'a> {
    fn from(s: &str) -> Box<dyn Error + 'a> {
        struct StrError(String);
        impl fmt::Display for StrError {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                write!(f, "{}", self.0)
            }
        }
        impl fmt::Debug for StrError {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                write!(f, "{}", self.0)
            }
        }
        impl Error for StrError {}
        Box::new(StrError(s.into()))
    }
}

impl<'a> From<String> for Box<dyn Error + 'a> {
    fn from(s: String) -> Box<dyn Error + 'a> {
        From::from(s.as_str())
    }
}

impl<'a> From<&str> for Box<dyn Error + Send + Sync + 'a> {
    fn from(s: &str) -> Box<dyn Error + Send + Sync + 'a> {
        struct StrError(String);
        impl fmt::Display for StrError {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                write!(f, "{}", self.0)
            }
        }
        impl fmt::Debug for StrError {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                write!(f, "{}", self.0)
            }
        }
        impl Error for StrError {}
        unsafe impl Send for StrError {}
        unsafe impl Sync for StrError {}
        Box::new(StrError(s.into()))
    }
}

impl Error for core::fmt::Error {}
impl Error for core::str::Utf8Error {}
impl Error for core::num::ParseIntError {}
impl Error for core::num::TryFromIntError {}
impl Error for core::char::CharTryFromError {}
impl Error for core::array::TryFromSliceError {}
impl Error for alloc::string::FromUtf8Error {}
impl Error for alloc::str::ParseBoolError {}
