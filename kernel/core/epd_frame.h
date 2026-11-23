/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef EPD_FRAME_H
#define EPD_FRAME_H

#include "epd_port.h"

struct epd_timing {
	u16 width, height, bitspanel;
	u16 lsl, lbl, lel, ldl;
	u16 fsl, fbl, fel;
	u16 gdck_sta, lgonl;
	u16 width_dummy_ldl, height_dummy;
	u8 sdsp_pulse_enable;
};

extern const struct epd_timing epd_timing_go7;

static inline u32 epd_lt(const struct epd_timing *t)
{
	return (u32)t->lsl + t->lbl + t->ldl + t->lel;
}

static inline u32 epd_rows(const struct epd_timing *t)
{
	return (u32)t->fsl + t->fbl + t->height + t->fel;
}

static inline size_t epd_frame_words(const struct epd_timing *t)
{
	return (size_t)epd_lt(t) * epd_rows(t);
}

static inline u32 epd_data_start(const struct epd_timing *t)
{
	u32 lt = epd_lt(t), lead = (u32)t->fbl + t->fsl + (t->height_dummy >> 1);

	return (u32)t->lbl - (t->lbl >> 1) + lt * lead + t->lbl + t->lsl + (t->width_dummy_ldl >> 1);
}

void epd_frame_template(const struct epd_timing *t, u32 *buf);
void epd_frame_encode(const struct epd_timing *t, const u32 *tmpl, const u8 *drive, u32 *buf);
void epd_frame_copy(const struct epd_timing *t, const u32 *src, u32 *dst, size_t pitch, bool swap);

#endif
