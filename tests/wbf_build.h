/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WBF_BUILD_H
#define WBF_BUILD_H

#include "epd_wbf.h"

#define WBF_END		0xff
#define WBF_TOGGLE	0xfc

struct wbf_spec {
	u8 mode_version;
	u8 lut_fmt;
	u8 adv_flags;
	u8 frame_rate;
	unsigned int n_modes, n_temps;
	const u8 *temp_bounds;
	const char *name;
	unsigned int (*frames)(unsigned int mode, unsigned int temp);
	u8 (*drive)(unsigned int mode, unsigned int temp, unsigned int frame, unsigned int nw, unsigned int old);
};

struct wbf_blob {
	u8 *data;
	size_t size;
	size_t mode_table;
};

void wbf_build(const struct wbf_spec *s, struct wbf_blob *out);
void wbf_fix_header(u8 *d);
size_t wbf_temp_table(const struct wbf_blob *b, unsigned int mode);
size_t wbf_block(const struct wbf_blob *b, unsigned int mode, unsigned int temp);
void wbf_set_ptr(u8 *d, size_t at, size_t target);
size_t wbf_grow(struct wbf_blob *b, size_t n);

#endif
