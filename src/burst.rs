//! Burst change detection:
//!  - short burst that changes enough of the panel (app switch?) gets GC16
//!  - longer one (scrolling?) does not

use crate::diff::Span;

/// Bursts longer than this many frames never get a GC16
pub const BIG_MAX_FRAMES: u32 = 10;

pub struct Burst {
    width: usize,
    height: usize,
    /// Changed span per (optional) row, over this burst
    rows: Vec<Option<Span>>,
    frames: u32,
}

impl Burst {
    pub fn new(width: usize, height: usize) -> Self {
        Self {
            width,
            height,
            rows: vec![None; height],
            frames: 0,
        }
    }

    pub fn reset(&mut self) {
        self.rows.fill(None);
        self.frames = 0;
    }

    /// Add a frame's changed rows to the burst, without counting the frame
    pub fn merge(&mut self, frame: &[Option<Span>]) {
        for (b, f) in self.rows.iter_mut().zip(frame) {
            if let Some(f) = *f {
                *b = Some(b.map_or(f, |b| b.union(f)));
            }
        }
    }

    /// Add a frame's changed rows, starting a new burst first if `new_burst`.
    ///
    /// True if the burst is short and has changed at least `pct` % of the panel,
    /// `pct` ≤ 0 is never and doesn't count the frame
    pub fn add(&mut self, frame: &[Option<Span>], new_burst: bool, pct: i32) -> bool {
        if new_burst {
            self.reset();
        }

        self.merge(frame);
        let pct = match usize::try_from(pct) {
            Ok(p) if p > 0 => p,
            _ => return false,
        };

        self.frames = self.frames.saturating_add(1);
        let area = self.rows.iter().flatten().fold(0, |a, s| s.width().saturating_add(a));
        self.frames <= BIG_MAX_FRAMES
            && area.saturating_mul(100) >= pct.saturating_mul(self.width.saturating_mul(self.height))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const W: usize = 10;
    const H: usize = 4;

    /// Rows 0..n changed over x 0..w
    fn rows(n: usize, w: usize) -> Vec<Option<Span>> {
        (0..H).map(|y| (y < n).then_some(Span { start: 0, end: w })).collect()
    }

    #[test]
    fn one_big_frame_is_big() {
        let mut b = Burst::new(W, H);
        assert!(b.add(&rows(2, W), true, 50));
        assert!(!Burst::new(W, H).add(&rows(1, W), true, 50));
    }

    #[test]
    fn small_frames_add_up() {
        let mut b = Burst::new(W, H);
        assert!(!b.add(&rows(1, W), true, 50));
        assert!(b.add(&[None, Some(Span { start: 0, end: W }), None, None], false, 50));
    }

    #[test]
    fn spans_in_a_row_are_merged() {
        let mut b = Burst::new(W, H);
        let at = |s, e| [Some(Span { start: s, end: e }), None, None, None];
        b.merge(&at(0, 2));
        b.merge(&at(6, 8));
        // one row 0..8 = 8 of 40 pixels = 20 %
        assert!(b.add(&at(2, 4), false, 20));
        assert!(!b.add(&at(2, 4), false, 21));
    }

    #[test]
    fn new_burst_starts_empty() {
        let mut b = Burst::new(W, H);
        assert!(!b.add(&rows(1, W), true, 50));
        assert!(!b.add(&rows(1, W), true, 50));
    }

    #[test]
    fn long_bursts_are_never_big() {
        let mut b = Burst::new(W, H);
        for i in 0..BIG_MAX_FRAMES {
            assert!(b.add(&rows(H, W), i == 0, 50));
        }
        assert!(!b.add(&rows(H, W), false, 50));
    }

    #[test]
    fn off_doesnt_count_frames() {
        let mut b = Burst::new(W, H);
        for i in 0..2 * BIG_MAX_FRAMES {
            assert!(!b.add(&rows(H, W), i == 0, 0));
        }
        assert!(b.add(&rows(H, W), false, 50));
        assert!(!b.add(&rows(H, W), false, -1));
    }

    #[test]
    fn reset_forgets_rows_and_frames() {
        let mut b = Burst::new(W, H);
        for i in 0..=BIG_MAX_FRAMES {
            assert_eq!(b.add(&rows(H, W), i == 0, 50), i < BIG_MAX_FRAMES);
        }
        b.reset();
        assert!(!b.add(&rows(1, W), false, 50));
        assert!(b.add(&rows(H, W), false, 50));
    }
}
