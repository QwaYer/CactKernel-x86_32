use super::{Error, ErrorKind, Read, Result, Write};
use alloc::vec::Vec;
use core::cmp;
use core::fmt;
use core::slice;

pub struct Cursor<T> {
    inner: T,
    pos: u64,
}

impl<T> Cursor<T> {
    pub fn new(inner: T) -> Self {
        Cursor { inner, pos: 0 }
    }

    pub fn into_inner(self) -> T {
        self.inner
    }

    pub fn get_ref(&self) -> &T {
        &self.inner
    }

    pub fn get_mut(&mut self) -> &mut T {
        &mut self.inner
    }

    pub fn position(&self) -> u64 {
        self.pos
    }

    pub fn set_position(&mut self, pos: u64) {
        self.pos = pos;
    }
}

impl Read for Cursor<&[u8]> {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
        let data = self.inner;
        let start = self.pos as usize;
        let n = cmp::min(buf.len(), data.len().saturating_sub(start));
        buf[..n].copy_from_slice(&data[start..start + n]);
        self.pos += n as u64;
        Ok(n)
    }
}

impl Read for Cursor<Vec<u8>> {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
        let data = self.inner.as_slice();
        let start = self.pos as usize;
        let n = cmp::min(buf.len(), data.len().saturating_sub(start));
        buf[..n].copy_from_slice(&data[start..start + n]);
        self.pos += n as u64;
        Ok(n)
    }
}

impl Read for Cursor<&mut [u8]> {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
        let data = self.inner.as_ref();
        let start = self.pos as usize;
        let n = cmp::min(buf.len(), data.len().saturating_sub(start));
        buf[..n].copy_from_slice(&data[start..start + n]);
        self.pos += n as u64;
        Ok(n)
    }
}

impl Write for Cursor<&mut [u8]> {
    fn write(&mut self, buf: &[u8]) -> Result<usize> {
        let n = cmp::min(buf.len(), self.inner.len().saturating_sub(self.pos as usize));
        self.inner[self.pos as usize..self.pos as usize + n].copy_from_slice(&buf[..n]);
        self.pos += n as u64;
        Ok(n)
    }
    fn flush(&mut self) -> Result<()> {
        Ok(())
    }
}

impl Write for Cursor<Vec<u8>> {
    fn write(&mut self, buf: &[u8]) -> Result<usize> {
        let start = self.pos as usize;
        if start + buf.len() > self.inner.len() {
            self.inner.resize(start + buf.len(), 0);
        }
        self.inner[start..start + buf.len()].copy_from_slice(buf);
        self.pos += buf.len() as u64;
        Ok(buf.len())
    }
    fn flush(&mut self) -> Result<()> {
        Ok(())
    }
}

pub struct Sink;
pub struct Empty;
pub struct Repeat { byte: u8 }

pub fn sink() -> Sink { Sink }
pub fn empty() -> Empty { Empty }
pub fn repeat(byte: u8) -> Repeat { Repeat { byte } }

impl Write for Sink {
    fn write(&mut self, buf: &[u8]) -> Result<usize> { Ok(buf.len()) }
    fn flush(&mut self) -> Result<()> { Ok(()) }
}

impl Read for Empty {
    fn read(&mut self, _buf: &mut [u8]) -> Result<usize> { Ok(0) }
}

impl Read for Repeat {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
        for b in buf.iter_mut() {
            *b = self.byte;
        }
        Ok(buf.len())
    }
}

pub fn copy<R: ?Sized + Read, W: ?Sized + Write>(reader: &mut R, writer: &mut W) -> Result<u64> {
    let mut buf = [0u8; 512];
    let mut total = 0;
    loop {
        match reader.read(&mut buf) {
            Ok(0) => return Ok(total),
            Ok(n) => {
                writer.write_all(&buf[..n])?;
                total += n as u64;
            }
            Err(ref e) if e.is_interrupted() => {}
            Err(e) => return Err(e),
        }
    }
}
