//! Tracking requests that the composer is building

/// What `drmModeAtomicAddProperty` should do with an add
#[derive(Debug, PartialEq, Eq)]
pub enum Add {
    /// One of the `EPDC_UPDATE_*` properties
    Swallow,

    /// Pass it on, after waiting for this input fence
    Forward { fence: Option<i32> },
}

/// Commit that needs e-ink updates added
#[derive(Debug, PartialEq, Eq)]
pub struct Ours {
    pub plane: u32,

    /// Framebuffer on the plane, 0 = detached
    pub fb: u32,

    pub cnt_prop: u32,
    pub parms_prop: u32,
}

/// Ids are 0 until seen
#[derive(Default)]
pub struct Tracker {
    cnt_prop: u32,
    parms_prop: u32,
    fence_prop: u32,
    fb_prop: u32,
    plane: u32,

    /// Address of the request being built, 0 = none
    req: usize,

    fb: u32,
    touches_plane: bool,
}

const fn is(prop: u32, id: u32) -> bool {
    prop != 0 && prop == id
}

/// C's `(int)value`, fd is the low 32 bits
fn fence_fd(value: u64) -> Option<i32> {
    let low = u32::try_from(value & 0xffff_ffff).ok()?;
    Some(low.cast_signed()).filter(|fd| *fd >= 0)
}

impl Tracker {
    pub const fn new() -> Self {
        Self {
            cnt_prop: 0,
            parms_prop: 0,
            fence_prop: 0,
            fb_prop: 0,
            plane: 0,
            req: 0,
            fb: 0,
            touches_plane: false,
        }
    }

    /// `drmModeGetProperty` returned property `id` called `name`
    pub fn learn_property(&mut self, name: &[u8], id: u32) {
        match name {
            b"EPDC_UPDATE_CNT" => self.cnt_prop = id,
            b"EPDC_UPDATE_PARMS_ADDR" => self.parms_prop = id,
            b"input_fence" => self.fence_prop = id,
            b"FB_ID" => self.fb_prop = id,
            _ => {}
        }
    }

    pub fn add(&mut self, req: usize, obj: u32, prop: u32, value: u64) -> Add {
        if req != self.req {
            self.req = req;
            self.fb = 0;
            self.touches_plane = false;
        }

        if is(prop, self.cnt_prop) || is(prop, self.parms_prop) {
            self.plane = obj;
            self.touches_plane = true;
            return Add::Swallow;
        }

        if self.plane != 0 && obj == self.plane {
            self.touches_plane = true;
        }

        if is(prop, self.fence_prop) {
            return Add::Forward { fence: fence_fd(value) };
        }

        if is(prop, self.fb_prop) && (self.plane == 0 || obj == self.plane) {
            self.fb = u32::try_from(value).unwrap_or(0);
        }

        Add::Forward { fence: None }
    }

    /// `drmModeAtomicSetCursor(req, 0)` restarts the request
    pub const fn set_cursor(&mut self, req: usize, cursor: i32) {
        if cursor == 0 && req == self.req {
            self.req = 0;
        }
    }

    /// The request is being committed, `Some` if it's ours
    pub fn commit(&mut self, req: usize, test_only: bool) -> Option<Ours> {
        let ours = req == self.req
            && self.touches_plane
            && self.plane != 0
            && self.cnt_prop != 0
            && self.parms_prop != 0
            && !test_only;

        if !test_only {
            self.req = 0;
        }

        ours.then_some(Ours {
            plane: self.plane,
            fb: self.fb,
            cnt_prop: self.cnt_prop,
            parms_prop: self.parms_prop,
        })
    }
}

#[cfg(test)]
#[expect(unused_results, reason = "adds and commits that only set up tracker state")]
mod tests {
    use super::*;

    const CNT: u32 = 10;
    const PARMS: u32 = 11;
    const FENCE: u32 = 12;
    const FB: u32 = 13;
    const PLANE: u32 = 40;
    const OTHER: u32 = 41;
    const REQ: usize = 0x1000;

    fn tracker() -> Tracker {
        let mut t = Tracker::new();
        for (name, id) in [
            (&b"EPDC_UPDATE_CNT"[..], CNT),
            (b"EPDC_UPDATE_PARMS_ADDR", PARMS),
            (b"input_fence", FENCE),
            (b"FB_ID", FB),
            (b"CRTC_ID", 99),
        ] {
            t.learn_property(name, id);
        }
        t
    }

    fn ours(fb: u32) -> Ours {
        Ours {
            plane: PLANE,
            fb,
            cnt_prop: CNT,
            parms_prop: PARMS,
        }
    }

    #[test]
    fn epdc_adds_are_swallowed_and_find_plane() {
        let mut t = tracker();
        assert_eq!(t.add(REQ, PLANE, CNT, 0), Add::Swallow);
        assert_eq!(t.add(REQ, PLANE, PARMS, 0), Add::Swallow);
        assert_eq!(t.add(REQ, PLANE, FB, 7), Add::Forward { fence: None });
        assert_eq!(t.commit(REQ, false), Some(ours(7)));
    }

    #[test]
    fn nothing_is_ours_before_ids_are_known() {
        let mut t = Tracker::new();
        assert_eq!(t.add(REQ, PLANE, CNT, 0), Add::Forward { fence: None });
        assert_eq!(t.commit(REQ, false), None);
    }

    #[test]
    fn fences_are_waited_on() {
        let mut t = tracker();
        assert_eq!(t.add(REQ, PLANE, FENCE, 5), Add::Forward { fence: Some(5) });
        assert_eq!(t.add(REQ, PLANE, FENCE, u64::MAX), Add::Forward { fence: None });
        assert_eq!(t.add(REQ, PLANE, FENCE, 0xffff_ffff), Add::Forward { fence: None });
        assert_eq!(t.add(REQ, PLANE, FENCE, 0x1_0000_0003), Add::Forward { fence: Some(3) });
    }

    #[test]
    fn later_requests_touch_plane_through_any_property() {
        let mut t = tracker();
        t.add(REQ, PLANE, CNT, 0);
        t.commit(REQ, false);
        t.add(REQ + 1, PLANE, FB, 8);
        assert_eq!(t.commit(REQ + 1, false), Some(ours(8)));
    }

    #[test]
    fn requests_not_touching_plane_are_not_ours() {
        let mut t = tracker();
        t.add(REQ, PLANE, CNT, 0);
        t.commit(REQ, false);
        t.add(REQ + 1, OTHER, FB, 8);
        assert_eq!(t.commit(REQ + 1, false), None);
    }

    #[test]
    fn fb_of_other_planes_ignored_once_plane_is_known() {
        let mut t = tracker();
        t.add(REQ, PLANE, CNT, 0);
        t.add(REQ, PLANE, FB, 7);
        t.add(REQ, OTHER, FB, 9);
        assert_eq!(t.commit(REQ, false), Some(ours(7)));
    }

    #[test]
    fn new_request_starts_clean() {
        let mut t = tracker();
        t.add(REQ, PLANE, CNT, 0);
        t.add(REQ, PLANE, FB, 7);
        t.add(REQ + 1, OTHER, 99, 1);
        assert_eq!(t.commit(REQ + 1, false), None);
    }

    #[test]
    fn test_only_commits_are_not_ours_and_keep_request() {
        let mut t = tracker();
        t.add(REQ, PLANE, CNT, 0);
        t.add(REQ, PLANE, FB, 7);
        assert_eq!(t.commit(REQ, true), None);
        assert_eq!(t.commit(REQ, false), Some(ours(7)));
        assert_eq!(t.commit(REQ, false), None);
    }

    #[test]
    fn resetting_cursor_ends_request() {
        let mut t = tracker();
        t.add(REQ, PLANE, CNT, 0);
        t.set_cursor(REQ, 3);
        t.set_cursor(REQ + 1, 0);
        assert_eq!(t.commit(REQ, true), None);
        t.set_cursor(REQ, 0);
        assert_eq!(t.commit(REQ, false), None);
    }

    #[test]
    fn detached_plane_has_fb_0() {
        let mut t = tracker();
        t.add(REQ, PLANE, CNT, 0);
        t.add(REQ, PLANE, FB, 0);
        assert_eq!(t.commit(REQ, false), Some(ours(0)));
    }
}
