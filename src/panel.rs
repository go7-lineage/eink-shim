//! Panel geometry

/// Panel width in pixels
pub const WIDTH: usize = 1264;
/// Panel height in pixels
pub const HEIGHT: usize = 1680;

/// A rectangle in panel pixels
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Rect {
    pub x: usize,
    pub y: usize,
    pub w: usize,
    pub h: usize,
}

impl Rect {
    /// Entire panel
    pub const FULL: Self = Self {
        x: 0,
        y: 0,
        w: WIDTH,
        h: HEIGHT,
    };
}
