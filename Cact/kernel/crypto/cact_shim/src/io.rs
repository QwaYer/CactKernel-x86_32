use core::fmt;
use alloc::boxed::Box;
use alloc::vec::Vec;

pub use self::error::{Error, ErrorKind, Result};
pub use self::impls::{Cursor, sink, empty, repeat, Sink, Empty, Repeat};

mod error;
mod impls;
mod util;

pub trait Read {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize>;

    fn read_to_end(&mut self, buf: &mut Vec<u8>) -> Result<usize> {
        let mut total = 0;
        let mut tmp = [0u8; 512];
        loop {
            match self.read(&mut tmp) {
                Ok(0) => break,
                Ok(n) => {
                    buf.extend_from_slice(&tmp[..n]);
                    total += n;
                }
                Err(ref e) if e.kind() == ErrorKind::Interrupted => {}
                Err(e) => return Err(e),
            }
        }
        Ok(total)
    }

    fn read_to_string(&mut self, buf: &mut alloc::string::String) -> Result<usize> {
        let mut bytes = Vec::new();
        let n = self.read_to_end(&mut bytes)?;
        buf.push_str(core::str::from_utf8(&bytes).map_err(|_| {
            Error::new(ErrorKind::InvalidData, "stream did not contain valid UTF-8")
        })?);
        Ok(n)
    }

    fn read_exact(&mut self, buf: &mut [u8]) -> Result<()> {
        let mut total = 0;
        while total < buf.len() {
            match self.read(&mut buf[total..]) {
                Ok(0) => {
                    return Err(Error::new(
                        ErrorKind::UnexpectedEof,
                        "failed to fill whole buffer",
                    ));
                }
                Ok(n) => total += n,
                Err(ref e) if e.kind() == ErrorKind::Interrupted => {}
                Err(e) => return Err(e),
            }
        }
        Ok(())
    }

    fn by_ref(&mut self) -> &mut Self
    where
        Self: Sized,
    {
        self
    }
}

pub trait Write {
    fn write(&mut self, buf: &[u8]) -> Result<usize>;
    fn flush(&mut self) -> Result<()>;

    fn write_all(&mut self, buf: &[u8]) -> Result<()> {
        let mut total = 0;
        while total < buf.len() {
            match self.write(&buf[total..]) {
                Ok(n) => total += n,
                Err(ref e) if e.kind() == ErrorKind::Interrupted => {}
                Err(e) => return Err(e),
            }
        }
        Ok(())
    }

    fn write_fmt(&mut self, fmt: fmt::Arguments<'_>) -> Result<()> {
        struct Adaptor<'a, T: ?Sized + 'a>(&'a mut T);
        impl<T: Write + ?Sized> fmt::Write for Adaptor<'_, T> {
            fn write_str(&mut self, s: &str) -> fmt::Result {
                self.0.write_all(s.as_bytes()).map_err(|_| fmt::Error)
            }
        }
        match core::fmt::write(&mut Adaptor(self), fmt) {
            Ok(()) => Ok(()),
            Err(_) => Err(Error::new(ErrorKind::Other, "write_fmt failed")),
        }
    }

    fn by_ref(&mut self) -> &mut Self
    where
        Self: Sized,
    {
        self
    }
}

pub trait BufRead: Read {
    fn fill_buf(&mut self) -> Result<&[u8]>;
    fn consume(&mut self, amt: usize);

    fn read_until(&mut self, byte: u8, buf: &mut Vec<u8>) -> Result<usize> {
        let mut total = 0;
        loop {
            let (avail, should_break) = {
                let avail = match self.fill_buf() {
                    Ok(n) => n,
                    Err(ref e) if e.kind() == ErrorKind::Interrupted => continue,
                    Err(e) => return Err(e),
                };
                if avail.is_empty() {
                    (0, true)
                } else {
                    let n = avail.iter().position(|&b| b == byte).map_or(avail.len(), |i| i + 1);
                    let do_break = n < avail.len() || avail.last() == Some(&byte);
                    buf.extend_from_slice(&avail[..n]);
                    (n, do_break)
                }
            };
            self.consume(avail);
            total += avail;
            if should_break {
                break;
            }
        }
        Ok(total)
    }

    fn read_line(&mut self, buf: &mut alloc::string::String) -> Result<usize> {
        let mut bytes = Vec::new();
        let n = self.read_until(b'\n', &mut bytes)?;
        buf.push_str(core::str::from_utf8(&bytes).map_err(|_| {
            Error::new(ErrorKind::InvalidData, "stream did not contain valid UTF-8")
        })?);
        if n > 0 && buf.ends_with('\n') {
            buf.pop();
            if buf.ends_with('\r') {
                buf.pop();
            }
        }
        Ok(n)
    }
}

impl Read for &[u8] {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
        let n = buf.len().min(self.len());
        let (a, b) = self.split_at(n);
        buf[..n].copy_from_slice(a);
        *self = b;
        Ok(n)
    }
}

impl Write for Vec<u8> {
    fn write(&mut self, buf: &[u8]) -> Result<usize> {
        self.extend_from_slice(buf);
        Ok(buf.len())
    }
    fn flush(&mut self) -> Result<()> {
        Ok(())
    }
}

impl Write for &mut [u8] {
    fn write(&mut self, buf: &[u8]) -> Result<usize> {
        let n = buf.len().min(self.len());
        self[..n].copy_from_slice(&buf[..n]);
        *self = &mut core::mem::take(self)[n..];
        Ok(n)
    }
    fn flush(&mut self) -> Result<()> {
        Ok(())
    }
}

/// Called by the `print!` / `println!` macros. Stub for kernel use.
pub fn _print(_s: &str) {}
