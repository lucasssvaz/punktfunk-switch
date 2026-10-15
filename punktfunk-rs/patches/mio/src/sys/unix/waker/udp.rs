use std::io;
use std::net::{Ipv4Addr, UdpSocket};
use std::os::fd::{AsRawFd, RawFd};

use crate::sys::Selector;
use crate::{Interest, Token};

/// Waker backed by a loopback UDP socket connected to itself.
///
/// Horizon on the Switch has BSD sockets but no `pipe(2)` or `eventfd(2)`, so a datagram to self
/// stands in for the pipe write; the socket is non-blocking and drained on reset.
#[derive(Debug)]
pub(crate) struct Waker {
    socket: UdpSocket,
}

impl Waker {
    #[allow(dead_code)] // Not used by the `poll(2)` implementation.
    pub(crate) fn new(selector: &Selector, token: Token) -> io::Result<Waker> {
        let waker = Waker::new_unregistered()?;
        selector.register(waker.socket.as_raw_fd(), token, Interest::READABLE)?;
        Ok(waker)
    }

    pub(crate) fn new_unregistered() -> io::Result<Waker> {
        let socket = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0))?;
        socket.connect(socket.local_addr()?)?;
        socket.set_nonblocking(true)?;
        Ok(Waker { socket })
    }

    pub(crate) fn wake(&self) -> io::Result<()> {
        // Loop, do not recurse: a full loopback send buffer under load used to
        // overflow the worker stack (`panic = abort` on Horizon).
        for _ in 0..8 {
            match self.socket.send(&[1]) {
                Ok(_) => return Ok(()),
                Err(ref err) if err.kind() == io::ErrorKind::WouldBlock => {
                    self.empty();
                }
                Err(ref err) if err.kind() == io::ErrorKind::Interrupted => {}
                Err(err) => return Err(err),
            }
        }
        Ok(())
    }

    #[allow(dead_code)] // Only used by the `poll(2)` implementation.
    pub(crate) fn ack_and_reset(&self) {
        self.empty();
    }

    #[allow(dead_code)] // Only used by the `poll(2)` implementation.
    pub(crate) fn fd(&self) -> Option<RawFd> {
        Some(self.socket.as_raw_fd())
    }

    #[allow(dead_code)] // Only used by the `poll(2)` implementation.
    pub(crate) fn woken(&self) -> bool {
        false
    }

    fn empty(&self) {
        let mut buf = [0; 64];
        while let Ok(_) = self.socket.recv(&mut buf) {}
    }
}

impl AsRawFd for Waker {
    fn as_raw_fd(&self) -> RawFd {
        self.socket.as_raw_fd()
    }
}
