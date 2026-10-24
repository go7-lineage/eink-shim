//! Reading a dma-buf from the CPU

use crate::abi::{DMA_BUF_IOCTL_SYNC, DMA_BUF_SYNC_END, DMA_BUF_SYNC_READ, DMA_BUF_SYNC_START};
use std::os::fd::{AsRawFd, BorrowedFd};
use std::ptr::{self, NonNull};

/// Read-only shared mapping, unmapped on drop
pub struct Mapping {
    ptr: NonNull<u8>,
    len: usize,
}

// SAFETY: a read-only mapping has no thread affinity
unsafe impl Send for Mapping {}

impl Mapping {
    /// Maps the first `len` bytes of `fd`, `None` if mmap fails
    pub fn new(fd: BorrowedFd<'_>, len: usize) -> Option<Self> {
        // SAFETY: fresh mapping at an address of kernel's choosing, nothing is overwritten
        let p = unsafe {
            libc::mmap(
                ptr::null_mut(),
                len,
                libc::PROT_READ,
                libc::MAP_SHARED,
                fd.as_raw_fd(),
                0,
            )
        };
        if p == libc::MAP_FAILED {
            return None;
        }

        Some(Self {
            ptr: NonNull::new(p.cast())?,
            len,
        })
    }

    pub const fn bytes(&self) -> &[u8] {
        // SAFETY: `len` mapped and readable bytes that live as long as `self`
        unsafe { std::slice::from_raw_parts(self.ptr.as_ptr(), self.len) }
    }
}

impl Drop for Mapping {
    fn drop(&mut self) {
        // Can only fail for a range that isn't mapped
        // SAFETY: unmaps exactly what `new` mapped, no borrows of `bytes()` outlive `self`
        let _ = unsafe { libc::munmap(self.ptr.as_ptr().cast(), self.len) };
    }
}

/// dma-buf reads: sync start on creation, sync end on drop
pub struct CpuRead<'a>(BorrowedFd<'a>);

impl<'a> CpuRead<'a> {
    pub fn begin(fd: BorrowedFd<'a>) -> Self {
        sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
        Self(fd)
    }
}

impl Drop for CpuRead<'_> {
    fn drop(&mut self) {
        sync(self.0, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    }
}

/// Best effort like the stock composer, failed sync still leaves the mapping readable
fn sync(fd: BorrowedFd<'_>, flags: u64) {
    // SAFETY: DMA_BUF_IOCTL_SYNC reads one u64 that outlives this call
    let _ = unsafe { libc::ioctl(fd.as_raw_fd(), DMA_BUF_IOCTL_SYNC, &raw const flags) };
}

#[cfg(test)]
pub mod tests {
    use super::*;
    use std::fs::{self, File, OpenOptions};
    use std::io::Write;
    use std::os::fd::AsFd;
    use std::sync::atomic::{AtomicU32, Ordering};

    /// Unlinked temporary file holding `data`
    pub fn buffer(data: &[u8]) -> File {
        static N: AtomicU32 = AtomicU32::new(0);
        let n = N.fetch_add(1, Ordering::Relaxed);
        let path = std::env::temp_dir().join(format!("eink_shim-test-{}-{n}", std::process::id()));
        let mut f = OpenOptions::new()
            .read(true)
            .write(true)
            .create_new(true)
            .open(&path)
            .unwrap();
        fs::remove_file(&path).unwrap();
        f.write_all(data).unwrap();
        f
    }

    #[test]
    fn maps_the_contents() {
        let f = buffer(b"0123456789");
        let m = Mapping::new(f.as_fd(), 10).unwrap();
        let _cpu = CpuRead::begin(f.as_fd());
        assert_eq!(m.bytes(), b"0123456789");
    }

    #[test]
    fn mapping_nothing_fails() {
        let f = buffer(b"x");
        assert!(Mapping::new(f.as_fd(), 0).is_none());
    }
}
