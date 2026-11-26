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

TEST(sleep_image_must_be_one_byte_per_pixel)
{
	CHECK(epd_sleep_image_ok(1264u * 1680u, 1264, 1680));
	CHECK(!epd_sleep_image_ok(1264u * 1680u - 1, 1264, 1680));
	CHECK(!epd_sleep_image_ok(1264u * 1680u + 1, 1264, 1680));
	CHECK(!epd_sleep_image_ok(1264u * 1680u * 4, 1264, 1680));
	CHECK(!epd_sleep_image_ok(0, 1264, 1680));
	CHECK(!epd_sleep_image_ok(0, 0, 1680));
	CHECK(!epd_sleep_image_ok(0, 1264, 0));
}

TEST(sleep_off_without_an_image_draws_nothing)
{
	struct epd_sleep s;

	memset(&s, 0, sizeof(s));
	CHECK(!epd_sleep_off(&s));
	CHECK(epd_sleep_can_resend(&s));
	CHECK_EQ(epd_sleep_frame(&s, true), EPD_SLEEP_SKIP);
	CHECK_EQ(epd_sleep_frame(&s, false), EPD_SLEEP_NORMAL);
	CHECK_EQ(s.n_shown, 0);
	CHECK_EQ(s.n_full, 0);
}

TEST(sleep_wake_after_the_image_refreshes_once)
{
	struct epd_sleep s;

	memset(&s, 0, sizeof(s));
	s.image = true;
	CHECK(epd_sleep_off(&s));
	CHECK_EQ(s.n_shown, 1);
	CHECK(!epd_sleep_can_resend(&s));
	CHECK_EQ(epd_sleep_frame(&s, true), EPD_SLEEP_SKIP);
	CHECK(!epd_sleep_can_resend(&s));
	CHECK_EQ(epd_sleep_frame(&s, false), EPD_SLEEP_FULL);
	CHECK(epd_sleep_can_resend(&s));
	CHECK_EQ(epd_sleep_frame(&s, false), EPD_SLEEP_NORMAL);
	CHECK_EQ(s.n_full, 1);
	CHECK_EQ(s.n_skipped, 1);
}

TEST(sleep_second_off_does_not_redraw)
{
	struct epd_sleep s;

	memset(&s, 0, sizeof(s));
	s.image = true;
	CHECK(epd_sleep_off(&s));
	CHECK(!epd_sleep_off(&s));
	CHECK_EQ(s.n_shown, 1);
	CHECK_EQ(epd_sleep_frame(&s, true), EPD_SLEEP_SKIP);
	CHECK_EQ(epd_sleep_frame(&s, false), EPD_SLEEP_FULL);
	CHECK(epd_sleep_off(&s));
	CHECK_EQ(s.n_shown, 2);
}

TEST(sleep_boot_modeset_with_an_image_is_skipped_without_a_refresh)
{
	struct epd_sleep s;

	memset(&s, 0, sizeof(s));
	s.image = true;
	CHECK_EQ(epd_sleep_frame(&s, true), EPD_SLEEP_SKIP);
	CHECK_EQ(epd_sleep_frame(&s, false), EPD_SLEEP_NORMAL);
	CHECK_EQ(s.n_full, 0);
}

TEST(sleep_off_during_shutdown_draws_nothing)
{
	struct epd_sleep s;

	memset(&s, 0, sizeof(s));
	s.image = true;
	s.shutdown = true;
	CHECK(!epd_sleep_off(&s));
	CHECK_EQ(s.n_shown, 0);
	CHECK(epd_sleep_can_resend(&s));
	CHECK_EQ(epd_sleep_frame(&s, false), EPD_SLEEP_NORMAL);
	s.shutdown = false;
	CHECK(epd_sleep_off(&s));
	CHECK_EQ(s.n_shown, 1);
}
