//! Plane formats

const KEY: &[u8] = b"pixel_formats=";
const LINEAR_32BPP: [&[u8]; 4] = [b"AB24", b"XB24", b"AR24", b"XR24"];

const fn is_format_char(b: u8) -> bool {
    b.is_ascii_alphanumeric() || b == b'_' || b == b'/'
}

/// Blank every format except linear 32b ones in each `pixel_formats=` line of capabilities blob.
/// Returns how many such lines list no linear 32-bit format
pub fn keep_linear_32bpp(blob: &mut [u8]) -> usize {
    let mut untouched: usize = 0;
    for line in blob.split_mut(|&b| b == b'\n') {
        let Some((key, list)) = line.split_at_mut_checked(KEY.len()) else {
            continue;
        };
        if key != KEY {
            continue;
        }

        let linear = |t: &[u8]| LINEAR_32BPP.contains(&t);
        if !list.split(|&b| !is_format_char(b)).any(linear) {
            untouched = untouched.saturating_add(1);
            continue;
        }

        for t in list.split_mut(|&b| !is_format_char(b)) {
            if !linear(t) {
                t.fill(b' ');
            }
        }
    }
    untouched
}

#[cfg(test)]
mod tests {
    use super::*;

    fn filter(s: &str) -> (String, usize) {
        let mut b = s.as_bytes().to_vec();
        let n = keep_linear_32bpp(&mut b);
        (String::from_utf8(b).unwrap(), n)
    }

    #[test]
    fn keeps_only_linear_32bpp() {
        assert_eq!(
            filter("pixel_formats=AB24 RG16 XB24/Q AR24 NV12 XR24\n"),
            (
                format!("pixel_formats=AB24{}AR24{}XR24\n", " ".repeat(13), " ".repeat(6)),
                0
            )
        );
    }

    #[test]
    fn other_lines_untouched() {
        let blob = "max_blendstages=1\nqseed_type=qseed3\npixel_formats_x=RG16\n pixel_formats=RG16\n";
        assert_eq!(filter(blob), (blob.to_owned(), 0));
    }

    #[test]
    fn list_without_lin_32bpp_is_left_and_counted() {
        let blob = "pixel_formats=RG16 NV12\npixel_formats=\npixel_formats=XB24 RG16";
        assert_eq!(
            filter(blob),
            (
                "pixel_formats=RG16 NV12\npixel_formats=\npixel_formats=XB24     ".to_owned(),
                2
            )
        );
    }

    #[test]
    fn separators_and_terminator_are_kept() {
        assert_eq!(
            filter("pixel_formats=RG16,AB24\0"),
            ("pixel_formats=    ,AB24\0".to_owned(), 0)
        );
    }
}
