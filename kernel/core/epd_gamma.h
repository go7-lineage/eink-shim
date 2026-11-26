/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef EPD_GAMMA_H
#define EPD_GAMMA_H

#include "epd_port.h"

struct epd_levels {
	u32 black, gamma, white;
};

#define EPD_LEVELS_IDENTITY	{ 0, 100, 255 }

int epd_levels_parse(const char *s, struct epd_levels *out);
bool epd_levels_identity(const struct epd_levels *l);
void epd_gamma_build(u8 *lut, const struct epd_levels *l);
bool epd_lut_build(u8 *lut, const struct epd_levels *l, bool invert);

#endif
