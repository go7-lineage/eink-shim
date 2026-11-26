/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef EPD_SLEEP_H
#define EPD_SLEEP_H

#include "epd_port.h"

enum epd_sleep_action { EPD_SLEEP_NORMAL, EPD_SLEEP_SKIP, EPD_SLEEP_FULL };

struct epd_sleep {
	bool image;
	bool shown;
	bool blank;
	bool shutdown;
	u64 n_shown, n_full, n_skipped;
};

bool epd_sleep_image_ok(size_t size, u32 w, u32 h);
bool epd_sleep_off(struct epd_sleep *s);
enum epd_sleep_action epd_sleep_frame(struct epd_sleep *s, bool turning_on);
bool epd_sleep_can_resend(const struct epd_sleep *s);

#endif
