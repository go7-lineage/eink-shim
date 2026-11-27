/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef EPD_CONVERT_H
#define EPD_CONVERT_H

#include "epd_port.h"

#define EPD_DITHER_FLAGS	0x6808
#define EPD_CONV_Y1_ORDERED	0x80000000u

void epd_rgba_to_grey_scalar(const u8 *src, size_t sstride, u8 *dst, size_t dstride, u32 w, u32 h, u8 mask, bool ggg,
			     bool bgr);
void epd_gamma(u8 *buf, size_t stride, u32 w, u32 h, const u8 *lut, u8 mask);
void epd_dither_y1(u8 *buf, u32 w, u32 h, size_t stride, int *err, int pix_level, bool grey_white);
void epd_dither_y4(u8 *buf, u32 w, u32 h, size_t stride, int *err);
void epd_dither_y5(u8 *buf, u32 w, u32 h, size_t stride, int *err);
void epd_dither_flyd(u8 *buf, u32 w, u32 h, size_t stride);
int epd_dither(u8 *buf, u32 w, u32 h, size_t stride, u32 flags, int *err);
void epd_dither_y1_ordered(u8 *buf, u32 w, u32 h, size_t stride, u32 x0, u32 y0);
void epd_regal_aa_mark(const u8 *levels, size_t stride, u32 w, u32 h, u8 *mark, int shift);
bool epd_quantize_update(u8 *grey, size_t stride, u32 left, u32 top, u32 w, u32 h, u32 flags, int *err);
void epd_convert_update(const u8 *rgba, size_t rgba_stride, u8 *grey, size_t grey_stride, u32 left,
			u32 top, u32 w, u32 h, bool ggg, bool bgr, const u8 *gamma);
void epd_convert_grey8(const u8 *src, size_t src_stride, u8 *grey, size_t grey_stride, u32 left, u32 top,
		       u32 w, u32 h, const u8 *gamma);

#endif
