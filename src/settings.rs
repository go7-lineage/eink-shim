//! User settings from `persist.vendor.eink_shim.*`

use crate::modes::{self, Mode};

pub const MODE: &str = "persist.vendor.eink_shim.mode";
pub const GC: &str = "persist.vendor.eink_shim.gc";
pub const FULL: &str = "persist.vendor.eink_shim.full";
pub const A2CUT: &str = "persist.vendor.eink_shim.a2cut";
pub const DEBUG: &str = "persist.vendor.eink_shim.debug";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Settings {
    pub mode: &'static Mode,

    /// Updates between full GC16 refreshes, 0 = never
    pub gc_interval: i32,

    /// % of the panel that a short burst must change to get GC16, 0 = off
    pub big_pct: i32,

    /// `cut_frame_num` for the current mode, 0..=9. `None` = leave alone
    pub cut: Option<u8>,

    pub debug: bool,
}

impl Settings {
    /// Read the settings through `get`, which returns a property's value. `None` if unset or empty
    pub fn read(get: impl Fn(&str) -> Option<String>) -> Self {
        let int = |name, default| get(name).and_then(|v| parse_int(&v)).unwrap_or(default);
        let mode = modes::by_name(get(MODE).as_deref().unwrap_or(""));
        Self {
            mode,
            gc_interval: int(GC, 20),
            big_pct: int(FULL, 50),
            cut: if mode.debounced {
                Some(0)
            } else {
                u8::try_from(int(A2CUT, 3).min(9)).ok()
            },
            debug: int(DEBUG, 0) != 0,
        }
    }
}

/// [`Settings`] cached against property area serial
#[derive(Default)]
pub struct Cached(Option<(u32, Settings)>);

impl Cached {
    /// Cached settings if `serial` hasn't changed, else the result of `read`
    pub fn get(&mut self, serial: u32, read: impl FnOnce() -> Settings) -> Settings {
        match self.0 {
            Some((s, settings)) if s == serial => settings,
            _ => {
                let settings = read();
                self.0 = Some((serial, settings));
                settings
            }
        }
    }
}

/// Parses all of `s` like C's `strtol(s, &end, 0)` with `*end == '\0'`. `None` if anything is left over
pub fn parse_int(s: &str) -> Option<i32> {
    let s = s.trim_start_matches([' ', '\t', '\n', '\x0b', '\x0c', '\r']);
    let (negative, s) = match s.strip_prefix('-') {
        Some(rest) => (true, rest),
        None => (false, s.strip_prefix('+').unwrap_or(s)),
    };
    let (radix, digits) = if let Some(hex) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        (16, hex)
    } else if let Some(oct) = s.strip_prefix('0').filter(|rest| !rest.is_empty()) {
        (8, oct)
    } else {
        (10, s)
    };
    if digits.is_empty() || !digits.chars().all(|c| c.is_digit(radix)) {
        return None;
    }
    let n = i64::from_str_radix(digits, radix).ok()?;
    i32::try_from(if negative { n.checked_neg()? } else { n }).ok()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    fn read(props: &[(&str, &str)]) -> Settings {
        let props: HashMap<_, _> = props.iter().copied().collect();
        Settings::read(|name| props.get(name).map(|v| (*v).to_owned()))
    }

    #[test]
    fn parses_like_strtol_base_0() {
        for (s, want) in [
            ("20", Some(20)),
            ("0", Some(0)),
            ("-3", Some(-3)),
            ("+7", Some(7)),
            ("  42", Some(42)),
            ("\t\x0b5", Some(5)),
            ("0x1F", Some(31)),
            ("0X10", Some(16)),
            ("-0x10", Some(-16)),
            ("010", Some(8)),
            ("00", Some(0)),
            ("", None),
            ("  ", None),
            ("-", None),
            ("0x", None),
            ("08", None),
            ("5 ", None),
            ("5x", None),
            ("+-5", None),
            ("--5", None),
            ("abc", None),
            ("99999999999", None),
        ] {
            assert_eq!(parse_int(s), want, "{s:?}");
        }
    }

    #[test]
    fn defaults() {
        let s = read(&[]);
        assert_eq!(s.mode.name, "speed");
        assert_eq!((s.gc_interval, s.big_pct, s.cut, s.debug), (20, 50, Some(3), false));
    }

    #[test]
    fn reads_every_property() {
        let s = read(&[(MODE, "regal"), (GC, "5"), (FULL, "0"), (A2CUT, "4"), (DEBUG, "1")]);
        assert_eq!(s.mode.name, "regal");
        assert_eq!((s.gc_interval, s.big_pct, s.debug), (5, 0, true));
    }

    #[test]
    fn bad_numbers_fall_back_to_defaults() {
        let s = read(&[(GC, "often"), (FULL, "50%"), (DEBUG, "yes")]);
        assert_eq!((s.gc_interval, s.big_pct, s.debug), (20, 50, false));
    }

    #[test]
    fn a2_cut_only_applies_to_speed() {
        assert_eq!(read(&[(A2CUT, "5")]).cut, Some(5));
        assert_eq!(read(&[(MODE, "normal"), (A2CUT, "5")]).cut, Some(0));
    }

    #[test]
    fn a2_cut_is_clamped_or_left_alone() {
        assert_eq!(read(&[(A2CUT, "12")]).cut, Some(9));
        assert_eq!(read(&[(A2CUT, "-1")]).cut, None);
    }

    #[test]
    fn cache_rereads_only_on_a_new_serial() {
        let mut cache = Cached::default();
        let speed = read(&[]);
        let regal = read(&[(MODE, "regal")]);
        assert_eq!(cache.get(1, || speed), speed);
        assert_eq!(cache.get(1, || regal), speed);
        assert_eq!(cache.get(2, || regal), regal);
    }
}
