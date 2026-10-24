//! Waiting on sync fence

use libc::c_int;

/// How long a commit waits for the GPU
pub const TIMEOUT_MS: c_int = 1000;

/// Wait until fence `fd` signals. False if it timed out, errors count as signalled
pub fn wait(fd: c_int, timeout_ms: c_int) -> bool {
    let mut p = libc::pollfd {
        fd,
        events: libc::POLLIN,
        revents: 0,
    };

    // SAFETY: one valid pollfd
    unsafe { libc::poll(&raw mut p, 1, timeout_ms) != 0 }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn signalled_and_timed_out() {
        let mut fds = [0; 2];
        // SAFETY: fds has room for both ends
        assert_eq!(unsafe { libc::pipe(fds.as_mut_ptr()) }, 0);
        let [r, w] = fds;
        assert!(!wait(r, 10));
        // SAFETY: w is the pipe's write, one byte from valid buffer
        assert_eq!(unsafe { libc::write(w, b"x".as_ptr().cast(), 1) }, 1);
        assert!(wait(r, 10));
        // SAFETY: both ends are ours and closed once
        assert_eq!(unsafe { libc::close(r) }, 0);
        // SAFETY: same as above
        assert_eq!(unsafe { libc::close(w) }, 0);
    }
}
