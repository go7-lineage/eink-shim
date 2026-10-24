//! A2 frame cut written on mode change

use crate::log::alogw;
use std::fs::OpenOptions;
use std::io::Write;

const CUT_FRAME_NUM: &str = "/sys/devices/virtual/sepdc/debug/cut_frame_num";

/// What to write for a cut of `n` frames: one digit, at most 9
pub fn line(n: u8) -> [u8; 2] {
    [b'0'.saturating_add(n.min(9)), b'\n']
}

/// Sets the cut, silent no-op if node can't be opened
pub fn write(n: u8) {
    let Ok(mut f) = OpenOptions::new().write(true).open(CUT_FRAME_NUM) else {
        return;
    };

    if let Err(e) = f.write_all(&line(n)) {
        alogw!("write cut_frame_num: {e}");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn one_digit_clamped() {
        assert_eq!(&line(0), b"0\n");
        assert_eq!(&line(3), b"3\n");
        assert_eq!(&line(9), b"9\n");
        assert_eq!(&line(200), b"9\n");
    }
}
