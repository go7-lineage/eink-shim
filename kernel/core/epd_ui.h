/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef EPD_UI_H
#define EPD_UI_H

#include "epd_policy.h"

enum epd_ui_mode { EPD_UI_SPEED, EPD_UI_NORMAL, EPD_UI_REGAL, EPD_UI_REGALPLUS, EPD_UI_NMODES };

#define EPD_UI_MAX_RECTS	4

struct epd_ui_cfg {
	int mode;
	int gc_interval;
	int big_pct;
	int a2cut;
};

struct epd_ui_span {
	u16 start, end;
};

struct epd_ui {
	u32 w, h;
	u64 *shadow;
	bool valid;
	struct epd_ui_span *rows;
	struct epd_ui_span *burst;
	struct epd_ui_span *transient;
	bool untracked;
	u32 burst_frames;
	bool have_mode, have_frame;
	int last_mode;
	u64 last_frame;
	int settled;
	int settle_mode;
	bool big_pending;
	int marker;
	bool flash_pending, have_flash;
	u64 last_flash;
	u32 flashes, flash_drops;
};

enum epd_ui_step { EPD_UI_IDLE, EPD_UI_WAIT, EPD_UI_SEND };

int epd_ui_init(struct epd_ui *u, u32 w, u32 h);
void epd_ui_release(struct epd_ui *u);
void epd_ui_invalidate(struct epd_ui *u);
int epd_ui_mode_by_name(const char *name);
const char *epd_ui_mode_name(int mode);
int epd_ui_frame(struct epd_ui *u, const struct epd_ui_cfg *cfg, const u8 *rgba, u32 pitch, size_t len,
		 u64 now, struct epd_request *out, int *cut);
enum epd_ui_step epd_ui_step(struct epd_ui *u, u64 now, struct epd_request *out, int *n, u64 *wait);
void epd_ui_gc_request(struct epd_request *out, u32 w, u32 h);
int epd_ui_flash(struct epd_ui *u, u64 now);

#endif
