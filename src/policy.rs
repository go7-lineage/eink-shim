//! Refresh policy for each changed frame

use crate::modes::Mode;
use std::time::{Duration, Instant};

/// Frames closer than this belong to one burst
pub const BURST: Duration = Duration::from_millis(100);

/// Cooldown time before settle pass
pub const SETTLE: Duration = Duration::from_millis(150);

/// Cooldown time before GC16
pub const BIG_QUIET: Duration = Duration::from_millis(400);

/// A frame after this much quiet counts towards `gc_interval`
pub const INTERACTION: Duration = Duration::from_millis(500);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    /// Full-panel GC16
    Gc16,

    /// A2, in debounced mode while content moving
    Transient,

    /// Changed area in the user's mode
    Normal,
}

#[derive(Debug, PartialEq, Eq)]
pub struct Decision {
    pub kind: Kind,

    /// Mode differs from the last frame's (also true for first frame)
    pub mode_changed: bool,

    /// Frame starts a new burst
    pub new_burst: bool,
}

#[derive(Default)]
pub struct Policy {
    last_mode: Option<&'static Mode>,
    last_frame: Option<Instant>,

    /// Interactions since the last GC16
    settled: i32,
}

impl Policy {
    /// Decide how to send a changed frame arriving at `now` in `mode`
    pub fn decide(&mut self, mode: &'static Mode, gc_interval: i32, now: Instant) -> Decision {
        let since = self.last_frame.map(|t| now.saturating_duration_since(t));
        let quiet_for = |d| since.is_none_or(|s| s >= d);

        self.last_frame = Some(now);
        let mut gc = false;

        let mode_changed = self.last_mode != Some(mode);
        if mode_changed {
            // Stock clears on a mode change
            gc = self.last_mode.is_some();
            self.last_mode = Some(mode);
            self.settled = 0;
        }

        if quiet_for(INTERACTION) && gc_interval > 0 {
            self.settled = self.settled.saturating_add(1);
            gc |= self.settled > gc_interval;
        }

        let kind = if gc {
            self.settled = 0;
            Kind::Gc16
        } else if mode.debounced && !quiet_for(BURST) {
            Kind::Transient
        } else {
            Kind::Normal
        };

        Decision {
            kind,
            mode_changed,
            new_burst: quiet_for(BIG_QUIET),
        }
    }
}

#[cfg(test)]
#[expect(unused_results, reason = "frames that only set up policy state")]
mod tests {
    use super::*;
    use crate::modes::by_name;

    struct Clock(Instant);
    impl Clock {
        fn after(&mut self, ms: u64) -> Instant {
            self.0 = self.0.checked_add(Duration::from_millis(ms)).unwrap();
            self.0
        }
    }

    fn setup() -> (Policy, Clock) {
        (Policy::default(), Clock(Instant::now()))
    }

    #[test]
    fn first_frame_sets_mode_without_gc() {
        let (mut p, mut t) = setup();
        let d = p.decide(by_name("normal"), 20, t.after(0));
        assert_eq!(
            d,
            Decision {
                kind: Kind::Normal,
                mode_changed: true,
                new_burst: true
            }
        );
        assert!(!p.decide(by_name("normal"), 20, t.after(1000)).mode_changed);
    }

    #[test]
    fn debounced_bursts_are_transient() {
        let (mut p, mut t) = setup();
        let normal = by_name("normal");
        p.decide(normal, 20, t.after(0));
        assert_eq!(p.decide(normal, 20, t.after(99)).kind, Kind::Transient);
        assert_eq!(p.decide(normal, 20, t.after(100)).kind, Kind::Normal);
    }

    #[test]
    fn speed_is_never_transient() {
        let (mut p, mut t) = setup();
        let speed = by_name("speed");
        p.decide(speed, 20, t.after(0));
        assert_eq!(p.decide(speed, 20, t.after(10)).kind, Kind::Normal);
    }

    #[test]
    fn mode_change_gcs_even_in_a_burst() {
        let (mut p, mut t) = setup();
        p.decide(by_name("speed"), 20, t.after(0));
        let d = p.decide(by_name("normal"), 20, t.after(10));
        assert_eq!((d.kind, d.mode_changed), (Kind::Gc16, true));
    }

    #[test]
    fn gc_every_interval_interactions() {
        let (mut p, mut t) = setup();
        let speed = by_name("speed");
        // the first frame is interaction 1
        let gcs: Vec<bool> = (0..9)
            .map(|_| p.decide(speed, 3, t.after(500)).kind == Kind::Gc16)
            .collect();
        assert_eq!(gcs, [false, false, false, true, false, false, false, true, false]);
    }

    #[test]
    fn frames_within_interaction_dont_count() {
        let (mut p, mut t) = setup();
        let speed = by_name("speed");
        p.decide(speed, 1, t.after(0));
        for _ in 0..10 {
            assert_eq!(p.decide(speed, 1, t.after(499)).kind, Kind::Normal);
        }
        assert_eq!(p.decide(speed, 1, t.after(500)).kind, Kind::Gc16);
    }

    #[test]
    fn gc_interval_0_is_never() {
        let (mut p, mut t) = setup();
        for _ in 0..50 {
            assert_eq!(p.decide(by_name("speed"), 0, t.after(1000)).kind, Kind::Normal);
        }
    }

    #[test]
    fn new_burst_after_big_quiet() {
        let (mut p, mut t) = setup();
        let speed = by_name("speed");
        p.decide(speed, 20, t.after(0));
        assert!(!p.decide(speed, 20, t.after(399)).new_burst);
        assert!(p.decide(speed, 20, t.after(400)).new_burst);
    }
}
