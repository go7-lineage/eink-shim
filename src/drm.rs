//! libdrm's ABI

use crate::log::aloge;
use libc::{c_int, c_ulong};
use std::ffi::{CStr, c_void};
use std::sync::OnceLock;

/// `drmModeAtomicReqPtr`
pub type AtomicReqPtr = *mut c_void;

/// `DRM_MODE_ATOMIC_TEST_ONLY` (`drm_mode.h`)
pub const ATOMIC_TEST_ONLY: u32 = 0x0100;

/// `DRM_PROP_NAME_LEN` (`drm_mode.h`)
pub const PROP_NAME_LEN: usize = 32;

/// Leading fields of `drmModePropertyRes` (`xf86drmMode.h`)
#[repr(C)]
pub struct Property {
    pub prop_id: u32,
    pub flags: u32,

    /// `char name[DRM_PROP_NAME_LEN]`
    pub name: [u8; PROP_NAME_LEN],
}

/// `drmModePropertyBlobRes` (`xf86drmMode.h`)
#[repr(C)]
pub struct PropertyBlob {
    pub id: u32,
    pub length: u32,
    pub data: *mut c_void,
}

pub type AtomicAddPropertyFn = unsafe extern "C" fn(AtomicReqPtr, u32, u32, u64) -> c_int;
pub type AtomicCommitFn = unsafe extern "C" fn(c_int, AtomicReqPtr, u32, *mut c_void) -> c_int;
pub type AtomicSetCursorFn = unsafe extern "C" fn(AtomicReqPtr, c_int);
pub type GetPropertyFn = unsafe extern "C" fn(c_int, u32) -> *mut Property;
pub type GetPropertyBlobFn = unsafe extern "C" fn(c_int, u32) -> *mut PropertyBlob;
pub type PrimeFdToHandleFn = unsafe extern "C" fn(c_int, c_int, *mut u32) -> c_int;
pub type IoctlFn = unsafe extern "C" fn(c_int, c_ulong, *mut c_void) -> c_int;

/// libdrm functions the hooks forward to
pub struct Real {
    pub atomic_add_property: AtomicAddPropertyFn,
    pub atomic_commit: AtomicCommitFn,
    pub atomic_set_cursor: AtomicSetCursorFn,
    pub get_property: GetPropertyFn,
    pub get_property_blob: GetPropertyBlobFn,
    pub prime_fd_to_handle: PrimeFdToHandleFn,
    pub ioctl: IoctlFn,
}

/// bionic dlfcn.h, libc crate lacks it for Android
#[cfg(all(target_os = "android", target_pointer_width = "64"))]
const RTLD_NEXT: *mut c_void = std::ptr::without_provenance_mut(usize::MAX);
#[cfg(all(target_os = "android", target_pointer_width = "32"))]
const RTLD_NEXT: *mut c_void = std::ptr::without_provenance_mut(0xffff_fffe);
#[cfg(not(target_os = "android"))]
const RTLD_NEXT: *mut c_void = libc::RTLD_NEXT;

impl Real {
    fn resolve() -> Result<Self, &'static CStr> {
        macro_rules! sym {
            ($name:literal, $ty:ty) => {{
                // SAFETY: lookup of a NUL-terminated name in libraries loaded after this one
                let p = unsafe { libc::dlsym(RTLD_NEXT, $name.as_ptr()) };
                if p.is_null() {
                    return Err($name);
                }

                // SAFETY: libdrm's function has this signature (`xf86drm.h`, `xf86drmMode.h`)
                // and function and data pointers have the same size here
                unsafe { std::mem::transmute::<*mut c_void, $ty>(p) }
            }};
        }

        Ok(Self {
            atomic_add_property: sym!(c"drmModeAtomicAddProperty", AtomicAddPropertyFn),
            atomic_commit: sym!(c"drmModeAtomicCommit", AtomicCommitFn),
            atomic_set_cursor: sym!(c"drmModeAtomicSetCursor", AtomicSetCursorFn),
            get_property: sym!(c"drmModeGetProperty", GetPropertyFn),
            get_property_blob: sym!(c"drmModeGetPropertyBlob", GetPropertyBlobFn),
            prime_fd_to_handle: sym!(c"drmPrimeFDToHandle", PrimeFdToHandleFn),
            ioctl: sym!(c"drmIoctl", IoctlFn),
        })
    }
}

static REAL: OnceLock<Real> = OnceLock::new();

/// Real libdrm functions. Abort if any one is missing
pub fn real() -> &'static Real {
    REAL.get_or_init(|| {
        Real::resolve().unwrap_or_else(|name| {
            aloge!("libdrm symbol {} missing", name.to_string_lossy());
            std::process::abort()
        })
    })
}
