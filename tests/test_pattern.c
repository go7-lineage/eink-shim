// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>
#include <string.h>

#include "epd_pattern.h"
#include "test.h"

#define W	1264
#define H	1680

static u8 reference(const char *name, int x, int y)
{
	if (!strcmp(name, "white"))
		return 0xff;
	if (!strcmp(name, "black"))
		return 0;
	if (!strcmp(name, "stripes"))
		return (u8)((x * 8 / W) * 0x24);
	if (!strcmp(name, "checker"))
		return ((x / 79) ^ (y / 105)) & 1 ? 0xff : 0;
	return (x + y) % 64 < 2 || (x - y + 64 * H) % 64 < 2 ? 0 : 0xff;
}

TEST(pattern_names)
{
	static const char *const names[] = { "white", "black", "stripes", "checker", "diagonal" };
	size_t i;

	for (i = 0; i < ARRAY_LEN(names); i++)
		CHECK_EQ(epd_pattern_by_name(names[i]), i);
	CHECK_EQ(epd_pattern_by_name("grey"), -EINVAL);
	CHECK_EQ(epd_pattern_by_name(""), -EINVAL);
	CHECK_EQ(epd_pattern_by_name("White"), -EINVAL);
}

TEST(pattern_go7_matches_reference)
{
	static const char *const names[] = { "white", "black", "stripes", "checker", "diagonal" };
	u32 *img = malloc((size_t)W * H * 4);
	size_t i;
	int x, y;

	for (i = 0; i < ARRAY_LEN(names); i++) {
		int bad = 0;

		epd_pattern_fill((enum epd_pattern)epd_pattern_by_name(names[i]), img, W, H);
		for (y = 0; y < H && !bad; y++)
			for (x = 0; x < W && !bad; x++) {
				u32 g = reference(names[i], x, y);

				if (img[y * W + x] != (0xff000000u | g << 16 | g << 8 | g)) {
					CHECK_EQ(img[y * W + x], 0xff000000u | g << 16 | g << 8 | g);
					bad = 1;
				}
			}
	}
	free(img);
}

TEST(pattern_stripes_are_eight_bars)
{
	u32 x, bars = 1;

	for (x = 1; x < W; x++)
		bars += epd_pattern_grey(EPD_PATTERN_STRIPES, x, 0, W, H) !=
			epd_pattern_grey(EPD_PATTERN_STRIPES, x - 1, 0, W, H);
	CHECK_EQ(bars, 8);
	CHECK_EQ(epd_pattern_grey(EPD_PATTERN_STRIPES, 0, 0, W, H), 0);
	CHECK_EQ(epd_pattern_grey(EPD_PATTERN_STRIPES, W - 1, 0, W, H), 0xfc);
}
