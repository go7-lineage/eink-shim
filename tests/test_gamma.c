// SPDX-License-Identifier: GPL-2.0-only
#include <math.h>

#include "epd_gamma.h"
#include "test.h"

static void check_levels(const char *s, u32 black, u32 gamma, u32 white)
{
	struct epd_levels l = { 1, 2, 3 };

	CHECK_EQ(epd_levels_parse(s, &l), 0);
	CHECK_EQ(l.black, black);
	CHECK_EQ(l.gamma, gamma);
	CHECK_EQ(l.white, white);
}

TEST(levels_parse_reads_black_gamma_white)
{
	check_levels("16 140 240", 16, 140, 240);
	check_levels("0 100 255\n", 0, 100, 255);
	check_levels(" 3\t77  200 \n", 3, 77, 200);
	check_levels("9 199 249", 9, 199, 249);
}

TEST(levels_parse_accepts_the_range_limits)
{
	check_levels("0 25 128", 0, 25, 128);
	check_levels("127 400 255", 127, 400, 255);
}

TEST(levels_parse_rejects_bad_input_and_keeps_the_old_values)
{
	static const char *const bad[] = {
		"", "\n", "16", "16 140", "16 140 240 1", "128 100 255", "0 100 127", "0 24 255", "0 401 255",
		"0 100 256", "a 100 255", "0 100 255x", "-1 100 255", "+1 100 255", "0 1.4 255", "0,100,255",
		"4294967312 100 255", "0 100 99999999999999999999",
	};
	struct epd_levels l = { 1, 2, 3 };
	size_t i;

	for (i = 0; i < ARRAY_LEN(bad); i++) {
		CHECK_EQ(epd_levels_parse(bad[i], &l), -EINVAL);
		CHECK_EQ(l.black, 1);
		CHECK_EQ(l.gamma, 2);
		CHECK_EQ(l.white, 3);
	}
}

TEST(levels_identity_is_only_0_100_255)
{
	const struct epd_levels id = EPD_LEVELS_IDENTITY, a = { 1, 100, 255 }, b = { 0, 101, 255 },
				c = { 0, 100, 254 };

	CHECK(epd_levels_identity(&id));
	CHECK(!epd_levels_identity(&a));
	CHECK(!epd_levels_identity(&b));
	CHECK(!epd_levels_identity(&c));
}

TEST(gamma_build_identity_maps_every_grey_to_itself)
{
	const struct epd_levels id = EPD_LEVELS_IDENTITY;
	u8 lut[256];
	int i;

	epd_gamma_build(lut, &id);
	for (i = 0; i < 256; i++)
		CHECK_EQ(lut[i], i);
}

TEST(gamma_build_clips_below_black_and_above_white)
{
	const struct epd_levels l = { 16, 100, 240 };
	u8 lut[256];
	int i;

	epd_gamma_build(lut, &l);
	for (i = 0; i <= 16; i++)
		CHECK_EQ(lut[i], 0);
	for (i = 240; i < 256; i++)
		CHECK_EQ(lut[i], 255);
	CHECK(lut[17] > 0);
	CHECK(lut[239] < 255);
}

TEST(gamma_above_one_darkens_mid_grey)
{
	const struct epd_levels dark = { 0, 140, 255 }, light = { 0, 70, 255 };
	u8 lut[256];

	epd_gamma_build(lut, &dark);
	CHECK(lut[128] < 128);
	epd_gamma_build(lut, &light);
	CHECK(lut[128] > 128);
}

static const u32 grid_black[] = { 0, 1, 16, 64, 127 };
static const u32 grid_gamma[] = { 25, 50, 73, 99, 100, 101, 140, 200, 333, 400 };
static const u32 grid_white[] = { 128, 129, 200, 254, 255 };

TEST(gamma_build_matches_libm_pow)
{
	struct epd_levels l;
	size_t b, g, w;
	u8 lut[256];
	int i;

	for (b = 0; b < ARRAY_LEN(grid_black); b++)
		for (g = 0; g < ARRAY_LEN(grid_gamma); g++)
			for (w = 0; w < ARRAY_LEN(grid_white); w++) {
				l = (struct epd_levels){ grid_black[b], grid_gamma[g], grid_white[w] };
				epd_gamma_build(lut, &l);
				for (i = 0; i < 256; i++) {
					double t = ((double)i - l.black) / (double)(l.white - l.black);
					double want = 255.0 * pow(t < 0 ? 0 : t > 1 ? 1 : t, l.gamma / 100.0);
					double frac = want - floor(want);

					if (fabs(frac - 0.5) < 1e-6)
						CHECK(fabs(lut[i] - want) <= 0.5 + 1e-6);
					else
						CHECK_EQ(lut[i], lround(want));
				}
			}
}

TEST(gamma_build_never_decreases)
{
	struct epd_levels l;
	size_t b, g, w;
	u8 lut[256];
	int i;

	for (b = 0; b < ARRAY_LEN(grid_black); b++)
		for (g = 0; g < ARRAY_LEN(grid_gamma); g++)
			for (w = 0; w < ARRAY_LEN(grid_white); w++) {
				l = (struct epd_levels){ grid_black[b], grid_gamma[g], grid_white[w] };
				epd_gamma_build(lut, &l);
				for (i = 1; i < 256; i++)
					CHECK(lut[i] >= lut[i - 1]);
			}
}
