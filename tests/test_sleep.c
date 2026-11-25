// SPDX-License-Identifier: GPL-2.0-only
#include <string.h>

#include "epd_sleep.h"
#include "test.h"

TEST(sleep_frame_that_turns_the_crtc_on_is_skipped)
{
	struct epd_sleep s;

	memset(&s, 0, sizeof(s));
	CHECK_EQ(epd_sleep_frame(&s, true), EPD_SLEEP_SKIP);
	CHECK_EQ(s.n_skipped, 1);
	CHECK_EQ(epd_sleep_frame(&s, true), EPD_SLEEP_SKIP);
	CHECK_EQ(s.n_skipped, 2);
}

TEST(sleep_frames_while_on_are_drawn)
{
	struct epd_sleep s;

	memset(&s, 0, sizeof(s));
	CHECK_EQ(epd_sleep_frame(&s, false), EPD_SLEEP_NORMAL);
	CHECK_EQ(epd_sleep_frame(&s, true), EPD_SLEEP_SKIP);
	CHECK_EQ(epd_sleep_frame(&s, false), EPD_SLEEP_NORMAL);
	CHECK_EQ(epd_sleep_frame(&s, false), EPD_SLEEP_NORMAL);
	CHECK_EQ(s.n_skipped, 1);
}

TEST(sleep_fresh_state_can_resend)
{
	struct epd_sleep s;

	memset(&s, 0, sizeof(s));
	CHECK(epd_sleep_can_resend(&s));
}

TEST(sleep_skipped_frame_blocks_resends_until_the_next_frame)
{
	struct epd_sleep s;

	memset(&s, 0, sizeof(s));
	CHECK_EQ(epd_sleep_frame(&s, true), EPD_SLEEP_SKIP);
	CHECK(!epd_sleep_can_resend(&s));
	CHECK_EQ(epd_sleep_frame(&s, false), EPD_SLEEP_NORMAL);
	CHECK(epd_sleep_can_resend(&s));
}
