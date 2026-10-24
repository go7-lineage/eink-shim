//! bionic's system properties, nothing is set on other hosts

/// bionic `PROP_VALUE_MAX` (`sys/system_properties.h`), including the NUL
#[cfg(target_os = "android")]
const PROP_VALUE_MAX: usize = 92;

#[cfg(target_os = "android")]
unsafe extern "C" {
    // Not in libc crate, bionic sys/system_properties.h
    fn __system_property_area_serial() -> u32;
}

/// The value of property `name`, `None` if unset or empty
#[cfg(target_os = "android")]
pub fn get(name: &str) -> Option<String> {
    let name = std::ffi::CString::new(name).ok()?;
    let mut value = [0u8; PROP_VALUE_MAX];
    // SAFETY: `name` is NUL-terminated and `value` has the PROP_VALUE_MAX bytes bionic may write
    let n = unsafe { libc::__system_property_get(name.as_ptr(), value.as_mut_ptr().cast()) };
    if n <= 0 {
        return None;
    }
    let value = std::ffi::CStr::from_bytes_until_nul(&value).ok()?;
    Some(value.to_string_lossy().into_owned())
}

/// The value of property `name`, `None` if unset or empty
#[cfg(not(target_os = "android"))]
pub const fn get(_name: &str) -> Option<String> {
    None
}

/// Counter that changes whenever any property changes
#[cfg(target_os = "android")]
pub fn area_serial() -> u32 {
    // SAFETY: no arguments, bionic returns serial of the mapped property area
    unsafe { __system_property_area_serial() }
}

/// Counter that changes whenever any property changes
#[cfg(not(target_os = "android"))]
pub const fn area_serial() -> u32 {
    0
}
