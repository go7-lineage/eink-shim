//! E-Ink updates sent to the kernel

use crate::abi::{EpdcUpd, STOCK_TEMP, UPDATE_FULL, UPDATE_PARTIAL};
use crate::modes::Mode;
use crate::panel::{self, Rect};

/// Update markers
#[derive(Default)]
pub struct Marker(i32);

impl Marker {
    pub const fn new() -> Self {
        Self(0)
    }

    pub fn next(&mut self) -> i32 {
        self.0 = self.0.wrapping_add(1).max(1);
        self.0
    }
}

fn px(v: usize) -> i32 {
    i32::try_from(v).unwrap_or(i32::MAX)
}

/// Partial update of `rect` in `mode`
pub fn partial(mode: &Mode, rect: Rect, marker: i32) -> EpdcUpd {
    EpdcUpd {
        rect: [px(rect.y), px(rect.x), px(rect.w), px(rect.h)],
        waveform_mode: mode.wf,
        update_mode: UPDATE_PARTIAL,
        update_marker: marker,
        temp: STOCK_TEMP,
        flag: mode.flag,
        reserved: 0,
    }
}

/// Full update over the whole panel with waveform `wf` and `flag`
pub fn full_panel(wf: i32, flag: i32, marker: i32) -> EpdcUpd {
    EpdcUpd {
        rect: [0, 0, px(panel::WIDTH), px(panel::HEIGHT)],
        waveform_mode: wf,
        update_mode: UPDATE_FULL,
        update_marker: marker,
        temp: STOCK_TEMP,
        flag,
        reserved: 0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::modes::{GC_FLAG, GC_WF, by_name};

    #[test]
    fn markers_count_up_and_stay_positive() {
        let mut m = Marker::new();
        assert_eq!((m.next(), m.next()), (1, 2));
        let mut m = Marker(i32::MAX - 1);
        assert_eq!((m.next(), m.next(), m.next()), (i32::MAX, 1, 2));
    }

    #[test]
    fn partial_rect_is_top_left_width_height() {
        let regal = by_name("regal");
        let u = partial(
            regal,
            Rect {
                x: 8,
                y: 30,
                w: 16,
                h: 2,
            },
            7,
        );
        assert_eq!(
            u,
            EpdcUpd {
                rect: [30, 8, 16, 2],
                waveform_mode: 5,
                update_mode: UPDATE_PARTIAL,
                update_marker: 7,
                temp: 20,
                flag: 0x1000,
                reserved: 0
            }
        );
    }

    #[test]
    fn full_panel_covers_the_panel() {
        let u = full_panel(GC_WF, GC_FLAG, 3);
        assert_eq!(u.rect, [0, 0, 1264, 1680]);
        assert_eq!(
            (u.waveform_mode, u.flag, u.update_mode, u.update_marker),
            (2, 0x1000, UPDATE_FULL, 3)
        );
    }
}
