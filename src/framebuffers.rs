//! Composer framebuffers

use crate::dmabuf::Mapping;
use std::os::fd::{AsFd, BorrowedFd, OwnedFd};
use std::sync::{Mutex, MutexGuard};

/// Max entries kept of each kind
pub const MAX: usize = 64;

struct Buf {
    handle: u32,
    fd: OwnedFd,
}

struct Fb {
    id: u32,
    pitch: u32,
    height: u32,
    fd: OwnedFd,

    /// Mapped on first read
    map: Option<Mapping>,
}

/// Framebuffer's pixels
pub struct Frame<'a> {
    pub bytes: &'a [u8],
    pub pitch: usize,
    pub fd: BorrowedFd<'a>,
}

#[derive(Default)]
pub struct Framebuffers {
    bufs: Vec<Buf>,
    fbs: Vec<Fb>,
}

static FRAMEBUFFERS: Mutex<Framebuffers> = Mutex::new(Framebuffers::new());

pub fn lock() -> MutexGuard<'static, Framebuffers> {
    crate::lock(&FRAMEBUFFERS)
}

impl Framebuffers {
    pub const fn new() -> Self {
        Self {
            bufs: Vec::new(),
            fbs: Vec::new(),
        }
    }

    /// GEM `handle` now refers to dma-buf `fd`
    pub fn imported(&mut self, handle: u32, fd: BorrowedFd<'_>) {
        self.gem_closed(handle);
        if self.bufs.len() < MAX
            && let Ok(fd) = fd.try_clone_to_owned()
        {
            self.bufs.push(Buf { handle, fd });
        }
    }

    pub fn gem_closed(&mut self, handle: u32) {
        self.bufs.retain(|b| b.handle != handle);
    }

    pub fn removed(&mut self, id: u32) {
        self.fbs.retain(|f| f.id != id);
    }

    /// ADDFB2 created framebuffer `id` on plane 0 of GEM `handle`
    pub fn added(&mut self, id: u32, handle: u32, pitch: u32, height: u32, modifier: u64) {
        self.removed(id);
        // tiled/compressed: not readable, full-panel fallback
        if id == 0 || modifier != 0 || self.fbs.len() >= MAX {
            return;
        }

        let Some(buf) = self.bufs.iter().find(|b| b.handle == handle) else {
            return;
        };

        if let Ok(fd) = buf.fd.try_clone() {
            self.fbs.push(Fb {
                id,
                pitch,
                height,
                fd,
                map: None,
            });
        }
    }

    /// Pixels of framebuffer `id`, `None` if it isn't known or can't be mapped
    pub fn frame(&mut self, id: u32) -> Option<Frame<'_>> {
        let fb = self.fbs.iter_mut().find(|f| f.id == id && id != 0)?;
        let pitch = usize::try_from(fb.pitch).ok()?;
        if fb.map.is_none() {
            let len = pitch.checked_mul(usize::try_from(fb.height).ok()?)?;
            fb.map = Some(Mapping::new(fb.fd.as_fd(), len)?);
        }

        Some(Frame {
            bytes: fb.map.as_ref()?.bytes(),
            pitch,
            fd: fb.fd.as_fd(),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::dmabuf::tests::buffer;

    /// 2*2 framebuffer with pitch 8, id 5 on handle 1
    fn setup() -> (Framebuffers, std::fs::File) {
        let file = buffer(&[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]);
        let mut fbs = Framebuffers::new();
        fbs.imported(1, file.as_fd());
        fbs.added(5, 1, 8, 2, 0);
        (fbs, file)
    }

    #[test]
    fn reads_added_framebuffer() {
        let (mut fbs, _file) = setup();
        let f = fbs.frame(5).unwrap();
        assert_eq!((f.bytes.len(), f.pitch, f.bytes[8]), (16, 8, 9));
        assert!(fbs.frame(6).is_none());
        assert!(fbs.frame(0).is_none());
    }

    #[test]
    fn removed_framebuffers_are_forgotten() {
        let (mut fbs, _file) = setup();
        fbs.removed(5);
        assert!(fbs.frame(5).is_none());
    }

    #[test]
    fn framebuffers_outlive_their_gem_handle() {
        let (mut fbs, _file) = setup();
        fbs.gem_closed(1);
        assert!(fbs.frame(5).is_some());
        fbs.added(6, 1, 8, 2, 0);
        assert!(fbs.frame(6).is_none());
    }

    #[test]
    fn tiled_and_unknown_buffers_not_tracked() {
        let (mut fbs, _file) = setup();
        fbs.added(6, 1, 8, 2, 0x0100_0000_0000_0001);
        fbs.added(7, 9, 8, 2, 0);
        assert!(fbs.frame(6).is_none());
        assert!(fbs.frame(7).is_none());
    }

    #[test]
    fn re_adding_id_replaces_it() {
        let (mut fbs, _file) = setup();
        fbs.added(5, 1, 4, 2, 0);
        assert_eq!(fbs.frame(5).unwrap().pitch, 4);
        assert_eq!(fbs.fbs.len(), 1);
    }

    #[test]
    fn re_importing_handle_replaces_it() {
        let (mut fbs, _file) = setup();
        let other = buffer(&[0xaa; 16]);
        fbs.imported(1, other.as_fd());
        fbs.added(6, 1, 8, 2, 0);
        assert_eq!(fbs.frame(6).unwrap().bytes[0], 0xaa);
        assert_eq!(fbs.bufs.len(), 1);
    }

    #[test]
    fn tables_are_capped() {
        let (mut fbs, file) = setup();
        for h in 2..=u32::try_from(MAX).unwrap() + 1 {
            fbs.imported(h, file.as_fd());
            fbs.added(h + 100, h, 8, 2, 0);
        }
        assert_eq!((fbs.bufs.len(), fbs.fbs.len()), (MAX, MAX));
    }
}
