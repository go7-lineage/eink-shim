/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef EPD_TABLES_H
#define EPD_TABLES_H

#include "epd_port.h"

struct epd_table_cfg {
	u8 cut;
	bool fill_zero;
	u8 a2_clean_mode;
	int a2_slot, du_slot;
};

int epd_tables4(const u8 *src, int nfr, int levels, int slot, const struct epd_table_cfg *c, u8 *full, u8 *part);
void epd_regal_fixup(u8 *frames, int n, int variant);
int epd_mode14_frames(u8 *frames, size_t cap);
u8 epd_a2_level(u8 target, u8 old);

#endif
