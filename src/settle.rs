//! Settle thread

use crate::abi::{EBC_SEND_UPDATE, EpdcUpd};
use crate::log::{self, aloge, alogi};
use crate::modes::{GC_FLAG, GC_WF, Mode};
use crate::policy::{BIG_QUIET, SETTLE};
use crate::updates::{self, Marker};
use std::fs::{File, OpenOptions};
use std::os::fd::AsRawFd;
use std::sync::{Condvar, Mutex, MutexGuard, PoisonError};
use std::thread;
use std::time::{Duration, Instant};

const EBC: &str = "/dev/ebc";

/// State shared between commit path and settle thread
pub struct Shared {
    pub last_frame: Option<Instant>,

    /// A2 frames went out and need a settle pass in this mode
    pub settle_mode: Option<&'static Mode>,

    /// Big burst needs a GC16, this wins over `settle_mode`
    pub big_pending: bool,

    /// Markers for both commit and settle updates
    pub marker: Marker,
}

#[derive(Debug, PartialEq, Eq)]
pub enum Step {
    /// Nothing pending, wait for a frame
    Idle,

    /// Frames may keep coming, check again after this
    Wait(Duration),

    /// Send this pass, pending work is cleared
    Send { update: EpdcUpd, big: bool },
}

impl Shared {
    pub const fn new() -> Self {
        Self {
            last_frame: None,
            settle_mode: None,
            big_pending: false,
            marker: Marker::new(),
        }
    }

    pub fn step(&mut self, now: Instant) -> Step {
        let (wf, flag, quiet) = match (self.big_pending, self.settle_mode) {
            (true, _) => (GC_WF, GC_FLAG, BIG_QUIET),
            (false, Some(m)) => (m.wf, m.flag, SETTLE),
            (false, None) => return Step::Idle,
        };

        if let Some(due) = self.last_frame.and_then(|t| t.checked_add(quiet))
            && now < due
        {
            return Step::Wait(due.saturating_duration_since(now));
        }

        let big = self.big_pending;
        self.settle_mode = None;
        self.big_pending = false;
        Step::Send {
            update: updates::full_panel(wf, flag, self.marker.next()),
            big,
        }
    }
}

static SHARED: Mutex<Shared> = Mutex::new(Shared::new());
static WAKE: Condvar = Condvar::new();

pub fn lock() -> MutexGuard<'static, Shared> {
    crate::lock(&SHARED)
}

/// Wake the thread after changing [`Shared`]
pub fn notify() {
    WAKE.notify_one();
}

/// Start the thread, false if it couldn't be
pub fn start() -> bool {
    thread::Builder::new()
        .name("eink_settle".into())
        .spawn(|| run())
        .is_ok()
}

fn run() -> ! {
    let ebc = OpenOptions::new()
        .read(true)
        .write(true)
        .open(EBC)
        .inspect_err(|e| aloge!("open {EBC}: {e}"))
        .ok();
    let mut shared = lock();

    loop {
        match shared.step(Instant::now()) {
            Step::Idle => shared = WAKE.wait(shared).unwrap_or_else(PoisonError::into_inner),
            Step::Wait(d) => shared = WAKE.wait_timeout(shared, d).unwrap_or_else(PoisonError::into_inner).0,
            Step::Send { update, big } => {
                drop(shared);
                let r = ebc.as_ref().map_or(-1, |f| send(f, &update));
                if r < 0 || log::debug() {
                    let kind = if big { "gc16" } else { "settle" };
                    alogi!("{kind} pass wf {} flag {:#x} -> {r}", update.waveform_mode, update.flag);
                }
                shared = lock();
            }
        }
    }
}

fn send(ebc: &File, update: &EpdcUpd) -> libc::c_int {
    // SAFETY: EBC_SEND_UPDATE reads one EpdcUpd, which outlives the call
    unsafe { libc::ioctl(ebc.as_raw_fd(), EBC_SEND_UPDATE, std::ptr::from_ref(update)) }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::modes::by_name;

    const MS: Duration = Duration::from_millis(1);

    #[test]
    fn idle_without_pending_work() {
        let mut s = Shared::new();
        s.last_frame = Some(Instant::now());
        assert_eq!(s.step(Instant::now()), Step::Idle);
    }

    #[test]
    fn settle_pass_after_settle_quiet() {
        let t0 = Instant::now();
        let regal = by_name("regal");
        let mut s = Shared::new();
        s.last_frame = Some(t0);
        s.settle_mode = Some(regal);
        assert_eq!(s.step(t0 + MS * 100), Step::Wait(MS * 50));
        let Step::Send { update, big } = s.step(t0 + MS * 150) else {
            panic!("no pass")
        };
        assert!(!big);
        assert_eq!(update, updates::full_panel(regal.wf, regal.flag, 1));
        assert_eq!(s.step(t0 + MS * 150), Step::Idle);
    }

    #[test]
    fn big_burst_gcs_after_big_quiet_and_wins() {
        let t0 = Instant::now();
        let mut s = Shared::new();
        s.last_frame = Some(t0);
        s.settle_mode = Some(by_name("normal"));
        s.big_pending = true;
        assert_eq!(s.step(t0 + MS * 150), Step::Wait(MS * 250));
        let Step::Send { update, big } = s.step(t0 + MS * 400) else {
            panic!("no pass")
        };
        assert!(big);
        assert_eq!(update, updates::full_panel(GC_WF, GC_FLAG, 1));
        assert_eq!((s.settle_mode, s.big_pending), (None, false));
    }

    #[test]
    fn passes_share_the_marker_sequence() {
        let mut s = Shared::new();
        assert_eq!(s.marker.next(), 1);
        s.big_pending = true;
        let Step::Send { update, .. } = s.step(Instant::now()) else {
            panic!("no pass")
        };
        assert_eq!(update.update_marker, 2);
    }
}
