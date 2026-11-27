/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef EPD_WBF_H
#define EPD_WBF_H

#include "epd_port.h"

#define EPD_MAX_FRAMES	1024
#define EPD_MODE_DU	1
#define EPD_MODE_GC16	2
#define EPD_MODE_A2	4
#define EPD_TEMP_HYST_MC	1000

struct epd_wbf {
	const u8 *data;
	size_t size;
	u8 mode_version;
	u8 frame_rate;
	u8 levels;		/* 16 or 32 */
	u16 n_modes, n_temps;
	u8 end_byte, toggle_byte;
	u8 adv_flags;		/* bit 7 enables the Regal plus fixup */
	u8 temp_bounds[257];
	char name[256];
	u32 *block;
	u16 *nframes;
};

int epd_wbf_parse(struct epd_wbf *w, const u8 *data, size_t size);
void epd_wbf_release(struct epd_wbf *w);
static inline int epd_wbf_frames(const struct epd_wbf *w, int temp, int mode)
{
	return w->nframes[mode * w->n_temps + temp];
}

int epd_wbf_decode(const struct epd_wbf *w, int temp, int mode, u8 *out, size_t cap);

int epd_temp_index(const struct epd_wbf *w, int temp_c);
int epd_temp_index_hyst(const struct epd_wbf *w, int cur, int temp_mc);
int epd_wbf_fixup_variant(const struct epd_wbf *w, int slot);
int epd_mode_slot(u8 mode_version, int onyx_mode);

static inline bool epd_mode_is_regal(int onyx_mode)
{
	return onyx_mode == 5 || onyx_mode == 15 || onyx_mode == 16;
}

static inline bool epd_mode_has_aa(int onyx_mode)
{
	return onyx_mode == 5 || onyx_mode == 16;
}

#endif
