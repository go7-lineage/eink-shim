/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef EPD_PATTERN_H
#define EPD_PATTERN_H

#include "epd_port.h"

enum epd_pattern {
	EPD_PATTERN_WHITE,
	EPD_PATTERN_BLACK,
	EPD_PATTERN_STRIPES,
	EPD_PATTERN_CHECKER,
	EPD_PATTERN_DIAGONAL,
	EPD_NPATTERNS
};

int epd_pattern_by_name(const char *name);
u8 epd_pattern_grey(enum epd_pattern pattern, u32 x, u32 y, u32 w, u32 h);
void epd_pattern_fill(enum epd_pattern pattern, u32 *xrgb, u32 w, u32 h);

#endif
