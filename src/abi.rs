//! Kernel ABI reverse-engineered from Onyx's EBC/EPDC driver

use libc::c_ulong;

/// One e-ink update, `EPDC_UPDATE_PARMS_ADDR` points at an array of these
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct EpdcUpd {
    /// top, left, width, height
    pub rect: [i32; 4],

    pub waveform_mode: i32,

    /// [`UPDATE_PARTIAL`] or [`UPDATE_FULL`]
    pub update_mode: i32,

    /// unique per submission
    pub update_marker: i32,

    /// Stock composer always sends [`STOCK_TEMP`], kernel uses fixed 24 deg C (`temp_deteck_enable` N)
    pub temp: i32,

    pub flag: i32,
    pub reserved: i32,
}

/// The kernel copies this many [`EpdcUpd`]-s from `EPDC_UPDATE_PARMS_ADDR`
pub const MAX_UPDATES: usize = 8;

pub const UPDATE_PARTIAL: i32 = 0;
pub const UPDATE_FULL: i32 = 1;
pub const STOCK_TEMP: i32 = 20;

/// Request argument of `ioctl(2)`: `int` in bionic and musl
#[cfg(any(target_os = "android", target_env = "musl"))]
pub type IoctlRequest = libc::c_int;

/// Request argument of `ioctl(2)`: `unsigned long` elsewhere
#[cfg(not(any(target_os = "android", target_env = "musl")))]
pub type IoctlRequest = c_ulong;

/// `/dev/ebc`: one [`EpdcUpd`] outside a commit
pub const EBC_SEND_UPDATE: IoctlRequest = 0x700c;

/// `DRM_IOCTL_MODE_ADDFB2`, argument [`FbCmd2`]
pub const IOCTL_ADDFB2: c_ulong = 0xc068_64b8;

/// `DRM_IOCTL_MODE_RMFB`, argument the fb id
pub const IOCTL_RMFB: c_ulong = 0xc004_64af;

/// msm's `DRM_IOCTL_MSM_RMFB2`, argument the fb id
pub const IOCTL_MSM_RMFB2: c_ulong = 0x4004_6483;

/// `DRM_IOCTL_GEM_CLOSE`, argument `struct drm_gem_close`, whose first field is the handle
pub const IOCTL_GEM_CLOSE: c_ulong = 0x4008_6409;

/// `DMA_BUF_IOCTL_SYNC`, argument a `u64` of `DMA_BUF_SYNC_*` flags
pub const DMA_BUF_IOCTL_SYNC: IoctlRequest = 0x4008_6200;

pub const DMA_BUF_SYNC_READ: u64 = 1;
pub const DMA_BUF_SYNC_START: u64 = 0;
pub const DMA_BUF_SYNC_END: u64 = 4;

/// `struct drm_mode_fb_cmd2`, argument of [`IOCTL_ADDFB2`]
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct FbCmd2 {
    pub fb_id: u32,
    pub width: u32,
    pub height: u32,
    pub pixel_format: u32,
    pub flags: u32,
    pub handles: [u32; 4],
    pub pitches: [u32; 4],
    pub offsets: [u32; 4],
    pub modifier: [u64; 4],
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::size_of;

    /// Linux `_IOC(dir, type, nr, size)`
    fn ioc(dir: u32, ty: u8, nr: u8, size: usize) -> c_ulong {
        let size = u32::try_from(size).unwrap();
        c_ulong::from(dir << 30 | size << 16 | u32::from(ty) << 8 | u32::from(nr))
    }
    const WRITE: u32 = 1;
    const READ_WRITE: u32 = 3;

    #[test]
    fn epdc_upd_is_40_bytes() {
        assert_eq!(size_of::<EpdcUpd>(), 40);
    }

    #[test]
    fn drm_ioctls_match_their_argument_layouts() {
        assert_eq!(size_of::<FbCmd2>(), 104);
        assert_eq!(IOCTL_ADDFB2, ioc(READ_WRITE, b'd', 0xb8, size_of::<FbCmd2>()));
        assert_eq!(IOCTL_RMFB, ioc(READ_WRITE, b'd', 0xaf, size_of::<u32>()));
        assert_eq!(IOCTL_MSM_RMFB2, ioc(WRITE, b'd', 0x83, size_of::<u32>()));
        // struct drm_gem_close { __u32 handle; __u32 pad; }
        assert_eq!(IOCTL_GEM_CLOSE, ioc(WRITE, b'd', 0x09, 2 * size_of::<u32>()));
    }

    #[test]
    fn dma_buf_sync_matches_its_argument_layout() {
        let req = c_ulong::try_from(DMA_BUF_IOCTL_SYNC).unwrap();
        assert_eq!(req, ioc(WRITE, b'b', 0, size_of::<u64>()));
    }
}
