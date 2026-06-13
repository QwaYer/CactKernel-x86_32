use alloc::string::String;
use alloc::boxed::Box;
use core::fmt;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ErrorKind {
    NotFound,
    PermissionDenied,
    ConnectionRefused,
    ConnectionReset,
    ConnectionAborted,
    NotConnected,
    AddrInUse,
    AddrNotAvailable,
    BrokenPipe,
    AlreadyExists,
    WouldBlock,
    InvalidInput,
    InvalidData,
    TimedOut,
    WriteZero,
    Interrupted,
    UnexpectedEof,
    Other,
}

pub struct Error {
    kind: ErrorKind,
    message: Option<String>,
}

impl Error {
    pub fn new<E>(kind: ErrorKind, error: E) -> Error
    where
        E: Into<Box<dyn crate::error::Error + Send + Sync>>,
    {
        Error {
            kind,
            message: Some(alloc::format!("{}", error.into())),
        }
    }

    pub fn kind(&self) -> ErrorKind {
        self.kind
    }

    pub fn is_interrupted(&self) -> bool {
        self.kind == ErrorKind::Interrupted
    }

    pub fn get_ref(&self) -> Option<&(dyn crate::error::Error + 'static)> {
        None
    }
}

impl fmt::Debug for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "io::Error(kind: {:?})", self.kind)
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if let Some(ref msg) = self.message {
            write!(f, "{}", msg)
        } else {
            write!(f, "{:?}", self.kind)
        }
    }
}

impl crate::error::Error for Error {
    fn source(&self) -> Option<&(dyn crate::error::Error + 'static)> {
        None
    }
}

impl From<ErrorKind> for Error {
    fn from(kind: ErrorKind) -> Error {
        Error {
            kind,
            message: None,
        }
    }
}

pub type Result<T> = core::result::Result<T, Error>;
