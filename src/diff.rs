//! Changed-area detection

use crate::panel::Rect;

/// A run of pixels `start..end` in one row
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Span {
    pub start: usize,
    pub end: usize,
}

impl Span {
    pub fn union(self, other: Self) -> Self {
        Self {
            start: self.start.min(other.start),
            end: self.end.max(other.end),
        }
    }

    pub const fn width(self) -> usize {
        self.end.saturating_sub(self.start)
    }
}

#[derive(Debug, PartialEq, Eq)]
pub enum Diff {
    Unchanged,

    /// Bounding box of the change, x aligned to 8px
    Changed(Rect),

    /// Frame too small for the panel, caller sends the full panel
    Unreadable,
}

pub struct Differ {
    width: usize,
    height: usize,

    /// u64 words per row
    words: usize,

    /// bytes of pixels per row
    row_bytes: usize,

    shadow: Vec<u64>,

    /// True iff `shadow` holds the last frame the panel got
    valid: bool,

    /// The last frame's changed span per row
    rows: Vec<Option<Span>>,
}

impl Differ {
    /// A differ for 32-bit frames of `width` (even) * `height` pixels
    pub fn new(width: usize, height: usize) -> Self {
        debug_assert!(width >= 2 && width.is_multiple_of(2));

        let words = width / 2;
        Self {
            width,
            height,
            words,
            row_bytes: words.saturating_mul(8),
            shadow: vec![0; words.saturating_mul(height)],
            valid: false,
            rows: vec![None; height],
        }
    }

    /// Last frame's changed span per row
    pub const fn rows(&self) -> &[Option<Span>] {
        self.rows.as_slice()
    }

    /// Discard shadow so the next frame counts as changed everywhere
    pub const fn invalidate(&mut self) {
        self.valid = false;
    }

    /// Compares `frame` (rows `pitch` bytes apart) with previous frame and remembers it
    pub fn diff(&mut self, frame: &[u8], pitch: usize) -> Diff {
        let row_bytes = self.row_bytes;
        if pitch < row_bytes || pitch.checked_mul(self.height).is_none_or(|n| frame.len() < n) {
            return Diff::Unreadable;
        }
        let word = |b: &[u8]| u64::from_ne_bytes(b.try_into().unwrap_or_default());

        // x span, first row, last row + 1
        let mut bbox: Option<(Span, usize, usize)> = None;
        let rows = frame
            .chunks(pitch)
            .zip(self.shadow.chunks_exact_mut(self.words))
            .zip(&mut self.rows);
        for (y, ((row, shadow), span)) in rows.enumerate() {
            let Some(src) = row.get(..row_bytes) else {
                self.valid = false;
                return Diff::Unreadable;
            };
            let (a, z) = if self.valid {
                let changed = |(s, d): (u64, &u64)| s != *d;
                let Some(a) = src.chunks_exact(8).map(word).zip(shadow.iter()).position(changed) else {
                    *span = None;
                    continue;
                };
                let z = src
                    .chunks_exact(8)
                    .map(word)
                    .zip(shadow.iter())
                    .rposition(changed)
                    .unwrap_or(a);
                (a, z)
            } else {
                (0, self.words.saturating_sub(1))
            };
            if let Some(dst) = shadow.get_mut(a..=z) {
                for (d, s) in dst.iter_mut().zip(src.chunks_exact(8).map(word).skip(a)) {
                    *d = s;
                }
            }

            // two pixels per word
            let s = Span {
                start: a.saturating_mul(2),
                end: z.saturating_add(1).saturating_mul(2),
            };
            *span = Some(s);
            let y1 = y.saturating_add(1);
            bbox = Some(bbox.map_or((s, y, y1), |(x, y0, _)| (x.union(s), y0, y1)));
        }

        self.valid = true;
        let Some((x, y0, y1)) = bbox else {
            return Diff::Unchanged;
        };
        let x0 = x.start & !7;
        let x1 = (x.end.saturating_add(7) & !7).min(self.width);

        Diff::Changed(Rect {
            x: x0,
            y: y0,
            w: x1.saturating_sub(x0),
            h: y1.saturating_sub(y0),
        })
    }
}

#[cfg(test)]
#[expect(clippy::arithmetic_side_effects, reason = "test frame geometry")]
mod tests {
    use super::*;

    const W: usize = 20;
    const H: usize = 4;

    /// W*H frame with `pitch` bytes per row, every pixel `fill`
    fn frame(pitch: usize, fill: u32) -> Vec<u8> {
        let mut f = vec![0xee; pitch * H];
        for row in f.chunks_mut(pitch) {
            for px in row[..W * 4].chunks_mut(4) {
                px.copy_from_slice(&fill.to_ne_bytes());
            }
        }
        f
    }

    fn set_px(f: &mut [u8], pitch: usize, x: usize, y: usize, v: u32) {
        let at = y * pitch + x * 4;
        f[at..at + 4].copy_from_slice(&v.to_ne_bytes());
    }

    fn primed(f: &[u8], pitch: usize) -> Differ {
        let mut d = Differ::new(W, H);
        assert!(matches!(d.diff(f, pitch), Diff::Changed(_)));
        d
    }

    #[test]
    fn first_frame_is_changed_everywhere() {
        let f = frame(W * 4, 0);
        let mut d = Differ::new(W, H);
        assert_eq!(d.diff(&f, W * 4), Diff::Changed(Rect { x: 0, y: 0, w: W, h: H }));
        assert!(d.rows().iter().all(|r| *r == Some(Span { start: 0, end: W })));
    }

    #[test]
    fn same_frame_is_unchanged() {
        let f = frame(W * 4, 7);
        let mut d = primed(&f, W * 4);
        assert_eq!(d.diff(&f, W * 4), Diff::Unchanged);
        assert!(d.rows().iter().all(Option::is_none));
    }

    #[test]
    fn one_pixel_gives_word_and_aligned_box() {
        let mut f = frame(W * 4, 0);
        let mut d = primed(&f, W * 4);
        set_px(&mut f, W * 4, 11, 2, 1);
        // pixel 11 is in word 5 = pixels 10..12, box x aligned out to 8..16
        assert_eq!(d.diff(&f, W * 4), Diff::Changed(Rect { x: 8, y: 2, w: 8, h: 1 }));
        assert_eq!(d.rows(), [None, None, Some(Span { start: 10, end: 12 }), None]);
    }

    #[test]
    fn box_covers_every_changed_row() {
        let mut f = frame(W * 4, 0);
        let mut d = primed(&f, W * 4);
        set_px(&mut f, W * 4, 2, 0, 1);
        set_px(&mut f, W * 4, 9, 3, 1);
        assert_eq!(
            d.diff(&f, W * 4),
            Diff::Changed(Rect {
                x: 0,
                y: 0,
                w: 16,
                h: 4
            })
        );
        assert_eq!(d.rows()[1], None);
    }

    #[test]
    fn box_clamped_to_panel() {
        let mut f = frame(W * 4, 0);
        let mut d = primed(&f, W * 4);
        set_px(&mut f, W * 4, W - 1, 1, 1);
        // 18..20 aligns out to 16..24, clamped to 16..20
        assert_eq!(
            d.diff(&f, W * 4),
            Diff::Changed(Rect {
                x: 16,
                y: 1,
                w: 4,
                h: 1
            })
        );
    }

    #[test]
    fn shadow_follows_frames() {
        let mut f = frame(W * 4, 0);
        let mut d = primed(&f, W * 4);
        set_px(&mut f, W * 4, 0, 0, 1);
        assert!(matches!(d.diff(&f, W * 4), Diff::Changed(_)));
        assert_eq!(d.diff(&f, W * 4), Diff::Unchanged);
        set_px(&mut f, W * 4, 0, 0, 0);
        assert!(matches!(d.diff(&f, W * 4), Diff::Changed(_)));
    }

    #[test]
    fn row_padding_is_ignored() {
        let pitch = W * 4 + 16;
        let mut f = frame(pitch, 0);
        let mut d = primed(&f, pitch);
        f[W * 4..pitch].fill(0x55);
        assert_eq!(d.diff(&f, pitch), Diff::Unchanged);
    }

    #[test]
    fn invalidate_makes_everything_changed() {
        let f = frame(W * 4, 0);
        let mut d = primed(&f, W * 4);
        d.invalidate();
        assert_eq!(d.diff(&f, W * 4), Diff::Changed(Rect { x: 0, y: 0, w: W, h: H }));
    }

    #[test]
    fn small_frames_are_unreadable() {
        let mut d = Differ::new(W, H);
        assert_eq!(d.diff(&frame(W * 4, 0)[..W * 4 * H - 1], W * 4), Diff::Unreadable);
        assert_eq!(d.diff(&vec![0; W * 4 * H], W * 4 - 8), Diff::Unreadable);
    }

    #[test]
    fn span_union_and_width() {
        let s = Span { start: 4, end: 8 }.union(Span { start: 2, end: 6 });
        assert_eq!(s, Span { start: 2, end: 8 });
        assert_eq!(s.width(), 6);
    }
}
