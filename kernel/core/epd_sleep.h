/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef EPD_SLEEP_H
#define EPD_SLEEP_H

#include "epd_port.h"

enum epd_sleep_action { EPD_SLEEP_NORMAL, EPD_SLEEP_SKIP };

struct epd_sleep {
	bool blank;
	u64 n_skipped;
};

enum epd_sleep_action epd_sleep_frame(struct epd_sleep *s, bool turning_on);
bool epd_sleep_can_resend(const struct epd_sleep *s);

#endif
