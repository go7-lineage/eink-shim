//! Per E-Ink commit: read settings, diff frame, apply refresh policy and
//! fill in the updates the kernel reads from `EPDC_UPDATE_PARMS_ADDR`

use std::sync::{LazyLock, Mutex, MutexGuard};
use std::time::Instant;

use crate::abi::{EpdcUpd, MAX_UPDATES};
use crate::burst::Burst;
use crate::diff::{Diff, Differ};
use crate::dmabuf::CpuRead;
use crate::log::{self, alogi};
use crate::modes::{A2, GC_FLAG, GC_WF};
use crate::panel::{HEIGHT, Rect, WIDTH};
use crate::policy::{Kind, Policy};
use crate::settings::{Cached, Settings};
use crate::updates::{full_panel, partial};
use crate::{cut, framebuffers, settle, sysprop};

pub struct Commit {
    settings: Cached,
    differ: Differ,
    burst: Burst,
    policy: Policy,

    /// `EPDC_UPDATE_PARMS_ADDR` points here
    updates: [EpdcUpd; MAX_UPDATES],

    settle_started: bool,
}

static COMMIT: LazyLock<Mutex<Commit>> = LazyLock::new(|| Mutex::new(Commit::new()));

/// Hold this across the real commit, so `updates` stays put while kernel reads it
pub fn lock() -> MutexGuard<'static, Commit> {
    crate::lock(&COMMIT)
}

impl Commit {
    fn new() -> Self {
        Self {
            settings: Cached::default(),
            differ: Differ::new(WIDTH, HEIGHT),
            burst: Burst::new(WIDTH, HEIGHT),
            policy: Policy::default(),
            updates: [EpdcUpd::default(); MAX_UPDATES],
            settle_started: false,
        }
    }

    /// Fills in the updates for framebuffer `fb` (0 = plane detached),
    /// Returns the values of `EPDC_UPDATE_PARMS_ADDR` and `EPDC_UPDATE_CNT`
    pub fn build(&mut self, fb: u32) -> (u64, u64) {
        let cnt = self.fill(fb);
        let addr = std::ptr::from_ref(&self.updates).expose_provenance();
        (u64::try_from(addr).unwrap_or(0), cnt)
    }

    /// Real commit failed (the panel never got this frame), so redraw everything next time
    pub const fn failed(&mut self) {
        self.differ.invalidate();
    }

    fn fill(&mut self, fb: u32) -> u64 {
        if fb == 0 {
            // screen off
            return 0;
        }

        let set = self.settings.get(sysprop::area_serial(), || {
            let set = Settings::read(sysprop::get);
            log::set_debug(set.debug);
            set
        });

        let diff = match framebuffers::lock().frame(fb) {
            Some(frame) => {
                let _cpu = CpuRead::begin(frame.fd);
                self.differ.diff(frame.bytes, frame.pitch)
            }
            None => Diff::Unreadable,
        };

        let (rect, tracked) = match diff {
            Diff::Unchanged => return 0,
            Diff::Changed(rect) => (rect, true),
            Diff::Unreadable => (Rect::FULL, false),
        };

        let now = Instant::now();
        let d = self.policy.decide(set.mode, set.gc_interval, now);
        if d.mode_changed
            && let Some(n) = set.cut
        {
            cut::write(n);
        }

        let mut shared = settle::lock();
        if !self.settle_started {
            self.settle_started = settle::start();
        }

        shared.last_frame = Some(now);
        match d.kind {
            Kind::Gc16 => {
                // GC16 frame already clears everything
                shared.settle_mode = None;
                shared.big_pending = false;
                self.burst.reset();
                if tracked {
                    self.burst.merge(self.differ.rows());
                }
            }
            Kind::Transient | Kind::Normal => {
                if d.kind == Kind::Transient {
                    shared.settle_mode = Some(set.mode);
                }
                if tracked {
                    // Re-evaluated per frame, a burst that turns into a scroll loses it
                    shared.big_pending = self.burst.add(self.differ.rows(), d.new_burst, set.big_pct);
                }
            }
        }

        let marker = shared.marker.next();
        self.updates[0] = match d.kind {
            Kind::Gc16 => full_panel(GC_WF, GC_FLAG, marker),
            Kind::Transient => partial(&A2, rect, marker),
            Kind::Normal => partial(set.mode, rect, marker),
        };

        drop(shared);
        settle::notify();

        if log::debug() {
            let name = match d.kind {
                Kind::Gc16 => "gc16",
                Kind::Transient => A2.name,
                Kind::Normal => set.mode.name,
            };
            let (r, u) = (rect, &self.updates[0]);
            alogi!(
                "update {name} rect {},{} {}x{} wf {} flag {:#x}",
                r.x,
                r.y,
                r.w,
                r.h,
                u.waveform_mode,
                u.flag
            );
        }
        1
    }
}
