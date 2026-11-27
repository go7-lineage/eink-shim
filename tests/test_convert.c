// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>
#include <string.h>

#include "epd_convert.h"
#include "epd_engine.h"
#include "epd_gamma.h"
#include "epd_neon.h"
#include "test.h"

static void randomize(u8 *buf, size_t n, unsigned int seed)
{
	size_t i;

	for (i = 0; i < n; i++)
		buf[i] = (u8)test_rand(&seed);
}

TEST(convert_grey_scalar_formula)
{
	u8 src[3 * 40], dst[3 * 9], want;
	u32 x, y;
	int ggg, bgr;

	randomize(src, sizeof(src), 1);
	for (ggg = 0; ggg < 2; ggg++)
		for (bgr = 0; bgr < 2; bgr++) {
			memset(dst, 0x11, sizeof(dst));
			epd_rgba_to_grey_scalar(src, 40, dst, 9, 7, 3, 0xf0, ggg, bgr);
			for (y = 0; y < 3; y++) {
				for (x = 0; x < 7; x++) {
					const u8 *p = src + y * 40 + 4 * x;
					u32 r = bgr ? p[2] : p[0], b = bgr ? p[0] : p[2];

					want = ggg ? p[0] : (u8)((77 * r + 151 * p[1] + 28 * b) >> 8);
					CHECK_EQ(dst[y * 9 + x], want & 0xf0);
				}
				CHECK_EQ(dst[y * 9 + 7], 0x11);
			}
		}
}

TEST(convert_grey_extremes)
{
	const u8 white[4] = { 255, 255, 255, 0 }, black[4] = { 0, 0, 0, 255 };
	u8 g;

	epd_rgba_to_grey_scalar(white, 4, &g, 1, 1, 1, 0xff, false, false);
	CHECK_EQ(g, 255);
	epd_rgba_to_grey_scalar(black, 4, &g, 1, 1, 1, 0xff, false, false);
	CHECK_EQ(g, 0);
}

TEST(convert_gamma_maps_through_lut)
{
	u8 lut[256], buf[2 * 5] = { 0, 1, 2, 3, 99, 255, 254, 253, 252, 99 };
	int i;

	for (i = 0; i < 256; i++)
		lut[i] = (u8)(255 - i);
	epd_gamma(buf, 5, 4, 2, lut, 0xff);
	CHECK_EQ(buf[0], 255);
	CHECK_EQ(buf[3], 252);
	CHECK_EQ(buf[4], 99);
	CHECK_EQ(buf[5], 0);
	CHECK_EQ(buf[8], 3);
	CHECK_EQ(buf[9], 99);
}

TEST(convert_gamma_masks_the_result)
{
	u8 lut[256], buf[3] = { 0, 0x2c, 0xff };
	int i;

	for (i = 0; i < 256; i++)
		lut[i] = (u8)(255 - i);
	epd_gamma(buf, 3, 3, 1, lut, 0xf0);
	CHECK_EQ(buf[0], 0xf0);
	CHECK_EQ(buf[1], 0xd0);
	CHECK_EQ(buf[2], 0);
}

struct size {
	u32 w, h;
};

static const struct size sizes[] = { { 1, 1 }, { 3, 2 }, { 4, 5 }, { 17, 9 }, { 64, 33 } };

#define MAXW	64
#define MAXH	33

static unsigned int run_dither(int which, const struct size *s, bool *in_set, const u8 *set, size_t nset)
{
	static u8 buf[MAXH * (MAXW + 3)];
	static int err[3 * MAXW + 12];
	size_t stride = s->w + 3, i;
	u32 x, y;

	randomize(buf, sizeof(buf), 7 + s->w);
	memset(err, 0, sizeof(err));
	switch (which) {
	case 0:
		epd_dither_y1(buf, s->w, s->h, stride, err, 0xff, false);
		break;
	case 1:
		epd_dither_y1(buf, s->w, s->h, stride, err, 0x80, true);
		break;
	case 2:
		epd_dither_y4(buf, s->w, s->h, stride, err);
		break;
	case 3:
		epd_dither_y5(buf, s->w, s->h, stride, err);
		break;
	default:
		epd_dither_flyd(buf, s->w, s->h, stride);
		break;
	}
	*in_set = true;
	for (y = 0; y < s->h; y++)
		for (x = 0; x < s->w; x++) {
			bool ok = false;

			for (i = 0; i < nset; i++)
				ok |= buf[y * stride + x] == set[i];
			*in_set &= ok;
		}
	return test_hash(buf, s->h * stride);
}

static void check_dither(int which, const unsigned int *pins, const u8 *set, size_t nset)
{
	size_t i;

	for (i = 0; i < ARRAY_LEN(sizes); i++) {
		bool in_set;

		CHECK_EQ(run_dither(which, &sizes[i], &in_set, set, nset), pins[i]);
		CHECK(in_set);
	}
}

static const u8 two_level[] = { 0, 0xff };
static const u8 sixteen_level[] = { 0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70,
				    0x80, 0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0 };
static const u8 y5_level[] = { 0, 36, 72, 108, 144, 180, 216, 252, 255 };
static const u8 flyd_level[] = { 0, 0xf0 };

TEST(convert_dither_y1_is_pinned)
{
	static const unsigned int pins[] = { 2540091573u, 571651678u, 2522218999u, 1461379099u, 4032501615u };

	check_dither(0, pins, two_level, sizeof(two_level));
}

TEST(convert_dither_y1_grey_white_is_pinned)
{
	static const unsigned int pins[] = { 2540091573u, 3406885724u, 3654732568u, 1404719409u, 1406573748u };

	check_dither(1, pins, two_level, sizeof(two_level));
}

TEST(convert_dither_y4_is_pinned)
{
	static const unsigned int pins[] = { 2632882901u, 3297039877u, 1538150877u, 955626081u, 2878535982u };

	check_dither(2, pins, sixteen_level, sizeof(sixteen_level));
}

TEST(convert_dither_y5_is_pinned)
{
	static const unsigned int pins[] = { 1260002225u, 159354049u, 2831936189u, 3562617406u, 4009129963u };

	check_dither(3, pins, y5_level, sizeof(y5_level));
}

TEST(convert_dither_flyd_is_pinned)
{
	static const unsigned int pins[] = { 2540091573u, 2455314181u, 1623213741u, 3160497169u, 3041127902u };

	check_dither(4, pins, flyd_level, sizeof(flyd_level));
}

TEST(convert_dither_keeps_solid_black_and_white)
{
	static u8 buf[8 * 8];
	static int err[3 * 8 + 12];
	u32 flags[] = { 0x2000, 0x4000, 0x0008 }, i, k;

	for (i = 0; i < ARRAY_LEN(flags); i++) {
		memset(buf, 0, sizeof(buf));
		CHECK_EQ(epd_dither(buf, 8, 8, 8, flags[i], err), 1);
		for (k = 0; k < sizeof(buf); k++)
			CHECK_EQ(buf[k], 0);
		memset(buf, 0xff, sizeof(buf));
		CHECK_EQ(epd_dither(buf, 8, 8, 8, flags[i], err), 1);
		for (k = 0; k < sizeof(buf); k++)
			CHECK(buf[k] >= 0xf0);
	}
}

TEST(convert_dither_flag_priority)
{
	static u8 a[16 * 16], b[16 * 16];
	static int err[3 * 16 + 12];

	randomize(a, sizeof(a), 3);
	memcpy(b, a, sizeof(b));
	CHECK_EQ(epd_dither(a, 16, 16, 16, 0x2000 | 0x4000 | 0x0800 | 0x0008, err), 1);
	memset(err, 0, sizeof(err));
	epd_dither_y1(b, 16, 16, 16, err, 0xff, false);
	CHECK_MEM(a, b, sizeof(a));

	randomize(a, sizeof(a), 3);
	memcpy(b, a, sizeof(b));
	CHECK_EQ(epd_dither(a, 16, 16, 16, 0x4000 | 0x0800 | 0x0008, err), 1);
	memset(err, 0, sizeof(err));
	epd_dither_y4(b, 16, 16, 16, err);
	CHECK_MEM(a, b, sizeof(a));

	randomize(a, sizeof(a), 3);
	memcpy(b, a, sizeof(b));
	CHECK_EQ(epd_dither(a, 16, 16, 16, 0x0800 | 0x0008, err), 1);
	epd_dither_flyd(b, 16, 16, 16);
	CHECK_MEM(a, b, sizeof(a));

	randomize(a, sizeof(a), 3);
	memcpy(b, a, sizeof(b));
	CHECK_EQ(epd_dither(a, 16, 16, 16, 0x0008, err), 1);
	memset(err, 0, sizeof(err));
	epd_dither_y5(b, 16, 16, 16, err);
	CHECK_MEM(a, b, sizeof(a));

	CHECK_EQ(epd_dither(a, 16, 16, 16, 0x1000, err), 0);
	CHECK_MEM(a, b, sizeof(a));
}

TEST(convert_ordered_dither_tiles_from_the_origin)
{
	static u8 full[40 * 24], part[40 * 24];
	u32 i, y;

	randomize(full, sizeof(full), 9);
	memcpy(part, full, sizeof(part));
	epd_dither_y1_ordered(full, 40, 24, 40, 0, 0);
	for (i = 0; i < sizeof(full); i++)
		CHECK(full[i] == 0 || full[i] == 0xff);
	epd_dither_y1_ordered(part + 5 * 40 + 11, 21, 13, 40, 11, 5);
	for (y = 5; y < 18; y++)
		CHECK_MEM(part + y * 40 + 11, full + y * 40 + 11, 21);
}

TEST(convert_ordered_dither_coverage_follows_grey)
{
	u8 tile[64];
	int g, prev = 0;
	u32 y, i;

	for (g = 0; g < 256; g++) {
		int white = 0;

		memset(tile, g, sizeof(tile));
		for (y = 0; y < 8; y++)
			epd_dither_y1_ordered(tile + y * 8, 8, 1, 8, 0, y);
		for (i = 0; i < 64; i++)
			white += tile[i] == 0xff;
		CHECK(white >= prev);
		CHECK(white * 255 / 64 <= g + 4 && white * 255 / 64 >= g - 8);
		prev = white;
	}
	CHECK_EQ(prev, 64);
}

static void aa_reference(const u8 *lv, size_t stride, u32 w, u32 h, u8 *mark, int shift)
{
	u32 x, y;
	int dx, dy;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			bool dark = false;

			for (dy = -1; dy <= 1; dy++)
				for (dx = -1; dx <= 1; dx++) {
					int yy = (int)y + dy, xx = (int)x + dx;

					if (xx < 0 || xx >= (int)w)
						continue;
					yy = yy < 0 ? 0 : yy >= (int)h ? (int)h - 1 : yy;
					dark |= lv[(size_t)yy * stride + (size_t)xx] >> shift < 15;
				}
			mark[(size_t)y * w + x] = w >= 2 && h >= 2 && dark && lv[y * stride + x] >> shift == 15;
		}
}

static void aa_levels(u8 *lv, size_t n, unsigned int seed)
{
	size_t i;

	for (i = 0; i < n; i++)
		lv[i] = test_rand(&seed) % 4 ? 0xf3 : (u8)test_rand(&seed);
}

TEST(convert_regal_aa_marks_white_next_to_dark)
{
	static const struct size aa_sizes[] = { { 1, 5 }, { 5, 1 }, { 2, 2 }, { 7, 3 }, { 16, 4 }, { 33, 6 } };
	static u8 lv[6 * 40], got[6 * 33 + 1], want[6 * 33 + 1];
	size_t i;

	for (i = 0; i < ARRAY_LEN(aa_sizes); i++) {
		u32 w = aa_sizes[i].w, h = aa_sizes[i].h;

		aa_levels(lv, sizeof(lv), 20 + (unsigned int)i);
		memset(got, 0xee, sizeof(got));
		epd_regal_aa_mark(lv, 40, w, h, got, 4);
		aa_reference(lv, 40, w, h, want, 4);
		CHECK_MEM(got, want, (size_t)w * h);
		CHECK_EQ(got[(size_t)w * h], 0xee);
	}
}

TEST(convert_update_stays_inside_the_rectangle)
{
	static u8 rgba[10 * 16 * 4], grey[10 * 16];
	u32 x, y;

	randomize(rgba, sizeof(rgba), 5);
	memset(grey, 0x5a, sizeof(grey));
	epd_convert_update(rgba, 16 * 4, grey, 16, 3, 2, 9, 5, false, false, NULL);
	for (y = 0; y < 10; y++)
		for (x = 0; x < 16; x++) {
			const u8 *p = rgba + (y * 16 + x) * 4;
			u8 want = (u8)((77 * p[0] + 151 * p[1] + 28 * p[2]) >> 8);
			bool in = x >= 3 && x < 12 && y >= 2 && y < 7;

			CHECK_EQ(grey[y * 16 + x], in ? want : 0x5a);
		}
}

TEST(convert_update_applies_gamma_to_every_grey)
{
	static u8 rgba[8 * 16 * 4], grey[8 * 16], want[8 * 16], lut[256];
	size_t i;

	for (i = 0; i < 256; i++)
		lut[i] = (u8)(255 - i);
	randomize(rgba, sizeof(rgba), 9);
	epd_convert_update(rgba, 16 * 4, grey, 16, 0, 0, 16, 8, false, true, lut);
	epd_rgba_to_grey_scalar(rgba, 16 * 4, want, 16, 16, 8, 0xff, false, true);
	for (i = 0; i < sizeof(want); i++)
		want[i] = lut[want[i]];
	CHECK_MEM(grey, want, sizeof(grey));
}

TEST(quantize_update_picks_the_dither_by_flag)
{
	static u8 src[8 * 16], grey[8 * 16], want[8 * 16];
	static int err[3 * 16 + 12], err2[3 * 16 + 12];

	randomize(src, sizeof(src), 8);
	memcpy(grey, src, sizeof(src));
	memcpy(want, src, sizeof(src));
	CHECK(epd_quantize_update(grey, 16, 0, 0, 16, 8, 0x2000 | EPD_CONV_Y1_ORDERED, err));
	epd_dither_y1_ordered(want, 16, 8, 16, 0, 0);
	CHECK_MEM(grey, want, sizeof(grey));

	memcpy(grey, src, sizeof(src));
	memcpy(want, src, sizeof(src));
	CHECK(epd_quantize_update(grey, 16, 0, 0, 16, 8, 0x2000, err));
	epd_dither_y1(want, 16, 8, 16, err2, 0xff, false);
	CHECK_MEM(grey, want, sizeof(grey));

	memcpy(grey, src, sizeof(src));
	memcpy(want, src, sizeof(src));
	memset(err2, 0, sizeof(err2));
	CHECK(epd_quantize_update(grey, 16, 0, 0, 16, 8, 0x4000 | EPD_CONV_Y1_ORDERED, err));
	epd_dither_y4(want, 16, 8, 16, err2);
	CHECK_MEM(grey, want, sizeof(grey));

	memcpy(grey, src, sizeof(src));
	CHECK(!epd_quantize_update(grey, 16, 0, 0, 16, 8, EPD_CONV_Y1_ORDERED | 0x1000, err));
	CHECK_MEM(grey, src, sizeof(grey));
}

TEST(quantize_update_stays_inside_the_rectangle)
{
	static const u32 flags[] = { 0x2000, 0x2000 | EPD_CONV_Y1_ORDERED, 0x4000, 0x0800, 0x0008 };
	static u8 src[10 * 16], grey[10 * 16];
	static int err[3 * 16 + 12];
	size_t f;
	u32 x, y;

	randomize(src, sizeof(src), 12);
	for (f = 0; f < ARRAY_LEN(flags); f++) {
		memcpy(grey, src, sizeof(src));
		CHECK(epd_quantize_update(grey, 16, 3, 2, 9, 5, flags[f], err));
		for (y = 0; y < 10; y++)
			for (x = 0; x < 16; x++)
				if (x < 3 || x >= 12 || y < 2 || y >= 7)
					CHECK_EQ(grey[y * 16 + x], src[y * 16 + x]);
	}
}

TEST(quantize_ordered_y1_is_the_same_for_any_rectangle)
{
	static u8 src[24 * 40], whole[24 * 40], part[24 * 40];
	static int err[3 * 40 + 12];
	u32 x, y;

	randomize(src, sizeof(src), 13);
	memcpy(whole, src, sizeof(src));
	CHECK(epd_quantize_update(whole, 40, 0, 0, 40, 24, 0x2000 | EPD_CONV_Y1_ORDERED, err));
	memcpy(part, src, sizeof(src));
	CHECK(epd_quantize_update(part, 40, 5, 3, 21, 4, 0x2000 | EPD_CONV_Y1_ORDERED, err));
	CHECK(epd_quantize_update(part, 40, 17, 9, 23, 11, 0x2000 | EPD_CONV_Y1_ORDERED, err));
	for (y = 0; y < 24; y++)
		for (x = 0; x < 40; x++) {
			bool in = (x >= 5 && x < 26 && y >= 3 && y < 7) || (x >= 17 && y >= 9 && y < 20);

			CHECK_EQ(part[y * 40 + x], in ? whole[y * 40 + x] : src[y * 40 + x]);
		}
}

TEST(convert_grey8_matches_rgba_of_the_same_grey)
{
	static u8 src[10 * 16], rgba[10 * 16 * 4], a[10 * 16], b[10 * 16], lut[256];
	size_t i;
	int g;

	randomize(src, sizeof(src), 11);
	for (i = 0; i < sizeof(src); i++)
		memset(rgba + 4 * i, src[i], 4);
	for (i = 0; i < 256; i++)
		lut[i] = (u8)(255 - i);
	for (g = 0; g < 2; g++) {
		memset(a, 0x5a, sizeof(a));
		memset(b, 0x5a, sizeof(b));
		epd_convert_update(rgba, 16 * 4, a, 16, 3, 2, 9, 5, true, false, g ? lut : NULL);
		epd_convert_grey8(src, 16, b, 16, 3, 2, 9, 5, g ? lut : NULL);
		CHECK_MEM(b, a, sizeof(a));
	}
}

#ifdef EPD_NEON
TEST(neon_grey_matches_scalar)
{
	static u8 src[5 * 200], a[5 * 50], b[5 * 50];
	u32 w;
	int ggg, bgr;

	randomize(src, sizeof(src), 11);
	for (w = 1; w <= 49; w++)
		for (ggg = 0; ggg < 2; ggg++)
			for (bgr = 0; bgr < 2; bgr++) {
				memset(a, 0x22, sizeof(a));
				memset(b, 0x22, sizeof(b));
				epd_rgba_to_grey_scalar(src, 200, a, 50, w, 5, 0xf0, ggg, bgr);
				epd_rgba_to_grey_neon(src, 200, b, 50, w, 5, 0xf0, ggg, bgr);
				CHECK_MEM(b, a, sizeof(a));
			}
}

TEST(neon_threshold_matches_scalar)
{
	u8 a[100], b[100], t16[16];
	u32 i, w;

	for (i = 0; i < 16; i++)
		t16[i] = (u8)(i * 16 + 3);
	for (w = 0; w <= 100; w++) {
		randomize(a, sizeof(a), 30 + w);
		memcpy(b, a, sizeof(b));
		epd_threshold16_neon(a, w, t16);
		for (i = 0; i < w; i++)
			b[i] = b[i] > t16[i & 15] ? 0xff : 0;
		CHECK_MEM(a, b, sizeof(a));
	}
}

TEST(neon_regal_aa_matches_scalar)
{
	static u8 lv[9 * 70], a[9 * 67], b[9 * 67];
	u32 w, h;

	for (h = 2; h <= 9; h += 7)
		for (w = 2; w <= 67; w++) {
			aa_levels(lv, sizeof(lv), 40 + w);
			memset(a, 0xee, sizeof(a));
			memset(b, 0xee, sizeof(b));
			epd_regal_aa_mark(lv, 70, w, h, a, 4);
			epd_regal_aa_mark_neon(lv, 70, w, h, b, 4);
			CHECK_MEM(b, a, sizeof(a));
		}
}

TEST(neon_row_matches_scalar)
{
	static u8 state[160], owner[160], tab[4][256], zero[256];
	const u8 *cur[EPD_IDLE + 1];
	u32 a[20], b[20], npix, i, k;
	unsigned int seed = 77;

	for (i = 0; i < 4; i++)
		for (k = 0; k < 256; k++)
			tab[i][k] = (u8)(test_rand(&seed) % 3);
	for (i = 0; i <= EPD_IDLE; i++)
		cur[i] = i < 4 ? tab[i] : zero;
	for (k = 0; k < 200; k++) {
		u32 run = 1 + test_rand(&seed) % 40;
		u8 o = 0;

		for (i = 0; i < 160; i++) {
			if (i % run == 0)
				o = test_rand(&seed) % 3 ? (u8)(test_rand(&seed) % 4) : EPD_IDLE;
			owner[i] = o;
			state[i] = (u8)test_rand(&seed);
		}
		npix = 8 * (1 + test_rand(&seed) % 20);
		for (i = 0; i < 20; i++)
			a[i] = b[i] = 0xabcd0000u | (test_rand(&seed) & 0xffff);
		epd_row_scalar(state, owner, cur, a, npix);
		epd_row_neon(state, owner, cur, b, npix);
		CHECK_MEM(b, a, sizeof(a));
	}
}
#endif

TEST(convert_update_inverts_through_the_table)
{
	static const u8 in[4] = { 0xff, 0x00, 0x80, 0x0f }, want[4] = { 0x00, 0xff, 0x7f, 0xf0 };
	static u8 rgba[4 * 4], grey[4], lut[256];
	size_t i;

	for (i = 0; i < 4; i++)
		memset(rgba + 4 * i, in[i], 3);
	CHECK(epd_lut_build(lut, NULL, true));
	epd_convert_update(rgba, 4 * 4, grey, 4, 0, 0, 4, 1, false, false, lut);
	CHECK_MEM(grey, want, sizeof(grey));
}
