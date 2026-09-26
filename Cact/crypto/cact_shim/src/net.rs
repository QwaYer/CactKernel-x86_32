pub mod tcp {
    use crate::io::{Read, Result, Write};

    pub struct TcpStream {
        sock: i32,
    }

    impl TcpStream {
        pub fn connect(_addr: &str) -> Result<TcpStream> {
            // Placeholder — in real use, cact_net TCP socket is wired through FFI
            Ok(TcpStream { sock: -1 })
        }

        pub fn try_clone(&self) -> Result<TcpStream> {
            Ok(TcpStream { sock: self.sock })
        }
    }

    impl Read for TcpStream {
        fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
            unsafe extern "C" {
                fn tcp_recv(sock: i32, buf: *mut u8, max_len: u16) -> i32;
            }
            if self.sock < 0 {
                return Err(crate::io::Error::new(
                    crate::io::ErrorKind::NotConnected,
                    "socket not connected",
                ));
            }
            let max = buf.len().min(u16::MAX as usize) as u16;
            // SAFETY: `buf` is a live `&mut [u8]` that the caller lent us for the
            // duration of the call, so `tcp_recv` may write at most `max` (<= its
            // length) bytes through the pointer, and does not retain it.
            let n = unsafe { tcp_recv(self.sock, buf.as_mut_ptr(), max) };
            if n < 0 {
                Err(crate::io::Error::new(
                    crate::io::ErrorKind::ConnectionReset,
                    "tcp_recv failed",
                ))
            } else {
                Ok(n as usize)
            }
        }
    }

    impl Write for TcpStream {
        fn write(&mut self, buf: &[u8]) -> Result<usize> {
            unsafe extern "C" {
                fn tcp_send(sock: i32, data: *mut u8, len: u16) -> i32;
            }
            if self.sock < 0 {
                return Err(crate::io::Error::new(
                    crate::io::ErrorKind::NotConnected,
                    "socket not connected",
                ));
            }
            let max = buf.len().min(u16::MAX as usize) as u16;
            // SAFETY: `buf` is a live `&[u8]` of at least `max` bytes; the kernel
            // `tcp_send` only reads that many bytes (the `*mut` in its C signature
            // is not used to mutate the buffer) and does not retain the pointer.
            let n = unsafe { tcp_send(self.sock, buf.as_ptr() as *mut u8, max) };
            if n < 0 {
                Err(crate::io::Error::new(
                    crate::io::ErrorKind::BrokenPipe,
                    "tcp_send failed",
                ))
            } else {
                Ok(n as usize)
            }
        }

        fn flush(&mut self) -> Result<()> {
            Ok(())
        }
    }
}

pub mod lookup_host {
    use alloc::vec::Vec;

    pub fn lookup_host(_host: &str) -> crate::io::Result<Vec<crate::net::SocketAddr>> {
        // Placeholder — real DNS via SYS_DNS_RESOLVE in cact_net
        Err(crate::io::Error::new(
            crate::io::ErrorKind::Other,
            "lookup_host not implemented in kernel",
        ))
    }
}

pub type SocketAddr = core::net::SocketAddrV4;

/// Stub TcpListener.
pub struct TcpListener;

impl TcpListener {
    pub fn bind(_addr: &str) -> crate::io::Result<TcpListener> {
        Err(crate::io::Error::new(
            crate::io::ErrorKind::Other,
            "TcpListener not implemented in kernel",
        ))
    }
}

pub mod addr {
    pub use core::net::{SocketAddrV4, SocketAddrV6, Ipv4Addr, Ipv6Addr};
}
