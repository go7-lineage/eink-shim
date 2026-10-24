//! E-Ink support shim for Boox Go 7, so that ``SurfaceFlinger`` could drive the panel
//!
//! Loaded into `vendor.qti.hardware.display.composer-service` as its first `DT_NEEDED` entry
//!

mod abi;
mod burst;
mod commit;
mod cut;
mod diff;
mod dmabuf;
mod drm;
mod exports;
mod fence;
mod formats;
mod framebuffers;
mod log;
mod modes;
mod panel;
mod policy;
mod request;
mod settings;
mod settle;
mod sysprop;
mod updates;

use std::sync::{Mutex, MutexGuard, PoisonError};

/// Locks `m` ignoring poisoning, panic aborts the process anyway
fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(PoisonError::into_inner)
}
