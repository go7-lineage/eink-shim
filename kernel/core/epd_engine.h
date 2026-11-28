/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef EPD_ENGINE_H
#define EPD_ENGINE_H

#include "epd_frame.h"
#include "epd_tables.h"
#include "epd_wbf.h"

#define EPD_NLUTS	127
#define EPD_IDLE	0x7f

struct epd_update {
	u16 top, left, width, height;
	int mode;
	bool full;
	bool repeat;
	u32 marker;
};

struct epd_engine_cfg {
	const struct epd_timing *timing;
	int temp_c;
	struct epd_table_cfg tables;
	void (*marker_done)(void *ctx, u32 marker);
	void *ctx;
	bool regal_aa;
	bool supersede;
};

struct epd_engine;
struct epd_engine *epd_engine_create(const struct epd_engine_cfg *cfg, const u8 *wbf, size_t size, int *err);
void epd_engine_destroy(struct epd_engine *e);
int epd_engine_set_temp_mc(struct epd_engine *e, int temp_mc);
void epd_engine_temp_range(const struct epd_engine *e, int *lo, int *hi);
int epd_engine_set_cut(struct epd_engine *e, int cut);
void epd_engine_set_aa_all(struct epd_engine *e, bool all);
int epd_engine_get_cut(const struct epd_engine *e);
int epd_engine_submit(struct epd_engine *e, const struct epd_update *u, const u8 *grey8, size_t stride);
bool epd_engine_busy(const struct epd_engine *e);
int epd_engine_tick(struct epd_engine *e, u32 *buf);
void epd_row_scalar(const u8 *state, const u8 *owner, const u8 *const *cur, u32 *out, u32 npix);
int epd_engine_mode_frames(struct epd_engine *e, int mode, bool full);
int epd_engine_frame_rate(const struct epd_engine *e);
int epd_engine_level(const struct epd_engine *e, u32 x, u32 y);

#endif
