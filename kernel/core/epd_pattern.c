// SPDX-License-Identifier: GPL-2.0-only
#include "epd_pattern.h"

static const char *const pattern_names[EPD_NPATTERNS] = {
	[EPD_PATTERN_WHITE] = "white",
	[EPD_PATTERN_BLACK] = "black",
	[EPD_PATTERN_STRIPES] = "stripes",
	[EPD_PATTERN_CHECKER] = "checker",
	[EPD_PATTERN_DIAGONAL] = "diagonal",
};

int epd_pattern_by_name(const char *name)
{
	int i;

	for (i = 0; i < EPD_NPATTERNS; i++)
		if (!strcmp(name, pattern_names[i]))
			return i;
	return -EINVAL;
}

u8 epd_pattern_grey(enum epd_pattern pattern, u32 x, u32 y, u32 w, u32 h)
{
	switch (pattern) {
	case EPD_PATTERN_BLACK:
		return 0;
	case EPD_PATTERN_STRIPES:
		return (u8)(x * 8 / w * 0x24);
	case EPD_PATTERN_CHECKER:
		return (x / (w / 16) ^ y / (h / 16)) & 1 ? 0xff : 0;
	case EPD_PATTERN_DIAGONAL:
		return (x + y) % 64 < 2 || (x + 64 * h - y) % 64 < 2 ? 0 : 0xff;
	case EPD_PATTERN_WHITE:
	case EPD_NPATTERNS:
		break;
	}
	return 0xff;
}

void epd_pattern_fill(enum epd_pattern pattern, u32 *xrgb, u32 w, u32 h)
{
	u32 x, y;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			u32 g = epd_pattern_grey(pattern, x, y, w, h);

			xrgb[(size_t)y * w + x] = 0xff000000u | g << 16 | g << 8 | g;
		}
}
