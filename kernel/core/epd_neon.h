/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef EPD_NEON_H
#define EPD_NEON_H

#include "epd_port.h"

void epd_row_neon(const u8 *state, const u8 *owner, const u8 *const *cur, u32 *out, u32 npix);
void epd_rgba_to_grey_neon(const u8 *src, size_t sstride, u8 *dst, size_t dstride, u32 w, u32 h, u8 mask, bool ggg,
			   bool bgr);
void epd_regal_aa_mark_neon(const u8 *levels, size_t stride, u32 w, u32 h, u8 *mark, int shift);
void epd_threshold16_neon(u8 *buf, u32 w, const u8 *t16);

#endif
