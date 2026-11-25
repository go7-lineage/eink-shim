// SPDX-License-Identifier: GPL-2.0-only
#include "epd_sleep.h"

bool epd_sleep_image_ok(size_t size, u32 w, u32 h)
{
	return w && h && size == (size_t)w * h;
}

bool epd_sleep_off(struct epd_sleep *s)
{
	if (!s->image || s->shown)
		return false;
	s->shown = true;
	s->n_shown++;
	return true;
}

enum epd_sleep_action epd_sleep_frame(struct epd_sleep *s, bool turning_on)
{
	s->blank = turning_on;
	if (turning_on) {
		s->n_skipped++;
		return EPD_SLEEP_SKIP;
	}
	if (s->shown) {
		s->shown = false;
		s->n_full++;
		return EPD_SLEEP_FULL;
	}
	return EPD_SLEEP_NORMAL;
}

bool epd_sleep_can_resend(const struct epd_sleep *s)
{
	return !s->blank && !s->shown;
}
