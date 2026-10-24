//! libdrm functions this shim defines in the composer

use crate::abi::{FbCmd2, IOCTL_ADDFB2, IOCTL_GEM_CLOSE, IOCTL_MSM_RMFB2, IOCTL_RMFB};
use crate::drm::{ATOMIC_TEST_ONLY, AtomicReqPtr, Property, PropertyBlob, real};
use crate::log::alogw;
use crate::request::{Add, Tracker};
use crate::{commit, fence, formats, framebuffers};
use libc::{c_int, c_ulong};
use std::ffi::c_void;
use std::os::fd::BorrowedFd;
use std::sync::{Mutex, MutexGuard};

static TRACKER: Mutex<Tracker> = Mutex::new(Tracker::new());

fn tracker() -> MutexGuard<'static, Tracker> {
    crate::lock(&TRACKER)
}

/// Learn the ids of the properties that other hooks look for
#[unsafe(no_mangle)]
pub unsafe extern "C" fn drmModeGetProperty(fd: c_int, id: u32) -> *mut Property {
    // SAFETY: forwarded as received
    let p = unsafe { (real().get_property)(fd, id) };

    // SAFETY: libdrm returns NULL or a valid property
    if let Some(prop) = unsafe { p.as_ref() } {
        let name = prop.name.split(|&b| b == 0).next().unwrap_or_default();
        tracker().learn_property(name, id);
    }
    p
}

/// Swallow composer's `EPDC_UPDATE_*` adds, wait on input fences, note the plane's fb
#[unsafe(no_mangle)]
pub unsafe extern "C" fn drmModeAtomicAddProperty(req: AtomicReqPtr, obj: u32, prop: u32, value: u64) -> c_int {
    if !req.is_null() {
        let add = tracker().add(req.addr(), obj, prop, value);
        match add {
            Add::Swallow => {
                // SAFETY: req is a live drmModeAtomicReq
                let cursor = unsafe { req.cast::<u32>().read() };
                return c_int::try_from(cursor).unwrap_or(c_int::MAX);
            }
            Add::Forward { fence: Some(fd) } => {
                if !fence::wait(fd, fence::TIMEOUT_MS) {
                    alogw!("input fence {fd}: GPU wait timed out");
                }
            }
            Add::Forward { fence: None } => {}
        }
    }
    // SAFETY: forwarded as received
    unsafe { (real().atomic_add_property)(req, obj, prop, value) }
}

/// Note the end of a request
#[unsafe(no_mangle)]
pub unsafe extern "C" fn drmModeAtomicSetCursor(req: AtomicReqPtr, cursor: c_int) {
    tracker().set_cursor(req.addr(), cursor);
    // SAFETY: forwarded as received
    unsafe { (real().atomic_set_cursor)(req, cursor) }
}

/// Keep only linear 32-bit formats in plane capability blobs
#[unsafe(no_mangle)]
pub unsafe extern "C" fn drmModeGetPropertyBlob(fd: c_int, id: u32) -> *mut PropertyBlob {
    // SAFETY: forwarded as received
    let b = unsafe { (real().get_property_blob)(fd, id) };
    // SAFETY: libdrm returns NULL or a valid blob
    if let Some(blob) = unsafe { b.as_ref() }
        && !blob.data.is_null()
        && let Ok(len) = usize::try_from(blob.length)
    {
        // SAFETY: blob is `len` bytes at `data`, allocated by libdrm and not yet shared
        let data = unsafe { std::slice::from_raw_parts_mut(blob.data.cast::<u8>(), len) };
        for _ in 0..formats::keep_linear_32bpp(data) {
            alogw!("plane lists no linear 32-bit format, left as is");
        }
    }
    b
}

/// Keep a duplicate of each imported dma-buf
#[unsafe(no_mangle)]
pub unsafe extern "C" fn drmPrimeFDToHandle(fd: c_int, prime_fd: c_int, handle: *mut u32) -> c_int {
    // SAFETY: forwarded as received
    let r = unsafe { (real().prime_fd_to_handle)(fd, prime_fd, handle) };
    if r == 0 && !handle.is_null() && prime_fd >= 0 {
        // SAFETY: libdrm just stored the handle there
        let handle = unsafe { handle.read() };
        // SAFETY: prime_fd is the caller's open dma-buf, only duplicated during this call
        let buf = unsafe { BorrowedFd::borrow_raw(prime_fd) };
        framebuffers::lock().imported(handle, buf);
    }
    r
}

/// Track framebuffers created and removed, and GEM handles closed
#[unsafe(no_mangle)]
pub unsafe extern "C" fn drmIoctl(fd: c_int, request: c_ulong, arg: *mut c_void) -> c_int {
    if !arg.is_null() && matches!(request, IOCTL_RMFB | IOCTL_MSM_RMFB2 | IOCTL_GEM_CLOSE) {
        // SAFETY: all three take a struct whose first field is u32
        let id = unsafe { arg.cast::<u32>().read_unaligned() };
        if request == IOCTL_GEM_CLOSE {
            framebuffers::lock().gem_closed(id);
        } else {
            framebuffers::lock().removed(id);
        }
    }
    // SAFETY: forwarded as received
    let r = unsafe { (real().ioctl)(fd, request, arg) };
    if r == 0 && request == IOCTL_ADDFB2 && !arg.is_null() {
        // SAFETY: ADDFB2 takes struct drm_mode_fb_cmd2, which the kernel just filled in
        let c = unsafe { arg.cast::<FbCmd2>().read_unaligned() };
        framebuffers::lock().added(c.fb_id, c.handles[0], c.pitches[0], c.height, c.modifier[0]);
    }
    r
}

/// Add the E-Ink updates to composer's commit on the E-Ink plane
#[unsafe(no_mangle)]
#[expect(clippy::significant_drop_tightening, reason = "kernel reads `parms` during commit")]
pub unsafe extern "C" fn drmModeAtomicCommit(
    fd: c_int,
    req: AtomicReqPtr,
    flags: u32,
    user_data: *mut c_void,
) -> c_int {
    let real = real();
    let ours = tracker().commit(req.addr(), flags & ATOMIC_TEST_ONLY != 0);
    let Some(ours) = ours else {
        // SAFETY: forwarded as received
        return unsafe { (real.atomic_commit)(fd, req, flags, user_data) };
    };
    let mut commit = commit::lock();
    let (parms, cnt) = commit.build(ours.fb);
    // SAFETY: req is the caller's live request, `parms` stays valid while `commit` is locked
    let added = unsafe { (real.atomic_add_property)(req, ours.plane, ours.parms_prop, parms) } >= 0
        // SAFETY: same as above
        && unsafe { (real.atomic_add_property)(req, ours.plane, ours.cnt_prop, cnt) } >= 0;
    if !added {
        // The plane keeps the last commit's values, so the panel may miss this frame
        commit.failed();
        alogw!("couldn't add the e-ink update properties");
    }
    // SAFETY: forwarded as received
    let r = unsafe { (real.atomic_commit)(fd, req, flags, user_data) };
    if r < 0 {
        commit.failed();
        alogw!("commit failed ({r}), flags {flags:#x}");
    }
    r
}
