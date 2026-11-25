// SPDX-License-Identifier: GPL-2.0-only
#include "epd_sleep.h"

enum epd_sleep_action epd_sleep_frame(struct epd_sleep *s, bool turning_on)
{
	s->blank = turning_on;
	if (turning_on) {
		s->n_skipped++;
		return EPD_SLEEP_SKIP;
	}
	return EPD_SLEEP_NORMAL;
}

bool epd_sleep_can_resend(const struct epd_sleep *s)
{
	return !s->blank;
}
