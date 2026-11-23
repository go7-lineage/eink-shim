/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef EPD_ROW_H
#define EPD_ROW_H

#include "epd_port.h"

static __always_inline void epd_row_pack(const u8 *state, const u8 *owner, const u8 *const *cur, u32 *out, u32 npix)
{
	u32 x, k;

	for (x = 0; x < npix; x += 8) {
		u32 w = 0;

		for (k = 0; k < 8; k++)
			w |= (u32)cur[owner[x + k]][state[x + k]] << (14 - 2 * k);
		out[x / 8] = (out[x / 8] & ~0xffffu) | w;
	}
}

#endif
