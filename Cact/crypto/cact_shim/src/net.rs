pub mod tcp {
    use crate::io::{Read, Result, Write};
    use alloc::vec::Vec;

    pub struct TcpStream {
        sock: i32,
        rx_buf: Vec<u8>,
    }

    impl TcpStream {
        pub fn connect(_addr: &str) -> Result<TcpStream> {
            // Placeholder — in real use, cact_net TCP socket is wired through FFI
            Ok(TcpStream {
                sock: -1,
                rx_buf: Vec::new(),
            })
        }

        pub fn try_clone(&self) -> Result<TcpStream> {
            Ok(TcpStream {
                sock: self.sock,
                rx_buf: Vec::new(),
            })
        }
    }

    impl Read for TcpStream {
        fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
            extern "C" {
                fn tcp_recv(sock: i32, buf: *mut u8, max_len: u16) -> i32;
            }
            if self.sock < 0 {
                return Err(crate::io::Error::new(
                    crate::io::ErrorKind::NotConnected,
                    "socket not connected",
                ));
            }
            let max = buf.len().min(u16::MAX as usize) as u16;
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
            extern "C" {
                fn tcp_send(sock: i32, data: *mut u8, len: u16) -> i32;
            }
            if self.sock < 0 {
                return Err(crate::io::Error::new(
                    crate::io::ErrorKind::NotConnected,
                    "socket not connected",
                ));
            }
            let max = buf.len().min(u16::MAX as usize) as u16;
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
