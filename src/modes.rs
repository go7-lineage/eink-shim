//! Refresh modes

/// Refresh mode selected by `persist.vendor.eink_shim.mode`
#[derive(Debug, PartialEq, Eq)]
pub struct Mode {
    pub name: &'static str,
    pub wf: i32,
    pub flag: i32,

    /// In this mode, bursts go out as A2 + one settle pass
    pub debounced: bool,
}

/// Selectable modes, first is default
pub static MODES: [Mode; 4] = [
    // UI_A2_QUALITY_MODE 0x904: A2 + kernel Y1 dither
    Mode {
        name: "speed",
        wf: 4,
        flag: 0x2000,
        debounced: false,
    },
    // UI_DEFAULT_MODE 5: kernel AUTO
    Mode {
        name: "normal",
        wf: 255,
        flag: 0,
        debounced: true,
    },
    // UI_REGAL_MODE 6
    Mode {
        name: "regal",
        wf: 5,
        flag: 0x1000,
        debounced: true,
    },
    // UI_REGAL_PLUS_MODE 9
    // TODO: only real with the stock waveform, test first
    Mode {
        name: "regalplus",
        wf: 16,
        flag: 0,
        debounced: true,
    },
];

/// What bursts in debounced mode go out as
pub static A2: Mode = Mode {
    name: "a2",
    wf: 4,
    flag: 0x2000,
    debounced: false,
};

/// Onyx `UI_GC_MODE` 0x62 = GC16 | FULL | WAIT → wf 2, full update, flag 0x1000
pub const GC_WF: i32 = 2;
pub const GC_FLAG: i32 = 0x1000;

/// Return mode by name, default if not found
pub fn by_name(name: &str) -> &'static Mode {
    MODES.iter().find(|m| m.name == name).unwrap_or(&MODES[0])
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finds_modes_by_name() {
        for m in &MODES {
            assert!(std::ptr::eq(by_name(m.name), m));
        }
    }

    #[test]
    fn unknown_or_empty_name_returns_default() {
        assert_eq!(by_name("").name, "speed");
        assert_eq!(by_name("Speed").name, "speed");
        assert_eq!(by_name("gc16").name, "speed");
    }
}
