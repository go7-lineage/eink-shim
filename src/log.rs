//! Logging to logcat on Android, stderr on others

use std::sync::atomic::{AtomicBool, Ordering};

static DEBUG: AtomicBool = AtomicBool::new(false);

/// Turn on or off per-update logging (`persist.vendor.eink_shim.debug`)
pub fn set_debug(on: bool) {
    DEBUG.store(on, Ordering::Relaxed);
}

pub fn debug() -> bool {
    DEBUG.load(Ordering::Relaxed)
}

#[derive(Clone, Copy)]
pub enum Priority {
    Info,
    Warn,
    Error,
}

#[cfg(target_os = "android")]
unsafe extern "C" {
    // liblog android/log.h
    fn __android_log_write(prio: libc::c_int, tag: *const libc::c_char, text: *const libc::c_char) -> libc::c_int;
}

/// Log line to logcat
#[cfg(target_os = "android")]
pub fn write(prio: Priority, msg: &str) {
    // android_LogPriority

    let prio = match prio {
        Priority::Info => 4,
        Priority::Warn => 5,
        Priority::Error => 6,
    };
    let text = std::ffi::CString::new(msg.replace('\0', "")).unwrap_or_default();

    // SAFETY: tag and text are NUL-terminated and outlive this call
    let _ = unsafe { __android_log_write(prio, c"eink_shim".as_ptr(), text.as_ptr()) };
}

/// Log line to stderr
#[cfg(not(target_os = "android"))]
pub fn write(prio: Priority, msg: &str) {
    let prio = match prio {
        Priority::Info => 'I',
        Priority::Warn => 'W',
        Priority::Error => 'E',
    };
    eprintln!("{prio} eink_shim: {msg}");
}

macro_rules! alogi {
    ($($arg:tt)*) => { $crate::log::write($crate::log::Priority::Info, &format!($($arg)*)) };
}
macro_rules! alogw {
    ($($arg:tt)*) => { $crate::log::write($crate::log::Priority::Warn, &format!($($arg)*)) };
}
macro_rules! aloge {
    ($($arg:tt)*) => { $crate::log::write($crate::log::Priority::Error, &format!($($arg)*)) };
}
pub(crate) use {aloge, alogi, alogw};
