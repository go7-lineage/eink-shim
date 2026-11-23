// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>
#include <string.h>

#include "fixture.h"
#include "test.h"

#define GDCLK	(1u << 17)
#define GDSP	(1u << 18)
#define SDSP	(1u << 19)
#define SDOE	(1u << 20)
#define GDOE	(1u << 21)
#define SDLE	(1u << 22)

static u32 fnv(const u32 *w, size_t n)
{
	u32 h = 2166136261u;
	size_t i;

	for (i = 0; i < n; i++) {
		h ^= w[i];
		h *= 16777619u;
	}
	return h;
}

TEST(frame_go7_geometry)
{
	const struct epd_timing *t = &epd_timing_go7;

	CHECK_EQ(epd_lt(t), 213);
	CHECK_EQ(epd_rows(t), 1699);
	CHECK_EQ(epd_frame_words(t), 213 * 1699);
	CHECK_EQ(epd_data_start(t), 5 + 213 * 5 + 25);
	CHECK_EQ(t->width / 8, t->ldl);
}

TEST(frame_go7_template_is_pinned)
{
	const struct epd_timing *t = &epd_timing_go7;
	size_t n = epd_frame_words(t), i;
	u32 *buf = malloc(n * sizeof(*buf)), any = 0;

	memset(buf, 0xff, n * sizeof(*buf));
	epd_frame_template(t, buf);
	for (i = 0; i < n; i++)
		any |= buf[i];
	CHECK_EQ(any & ~(GDCLK | GDSP | SDSP | SDOE | GDOE | SDLE), 0);
	CHECK_EQ(fnv(buf, n), 0x3a6933c7);
	free(buf);
}

static u32 count(const u32 *buf, size_t from, size_t to, u32 bit)
{
	u32 n = 0;

	for (; from < to; from++)
		n += !!(buf[from] & bit);
	return n;
}

TEST(frame_template_structure)
{
	const struct epd_timing *t = &fixture_timing;
	u32 lt = epd_lt(t), rows = epd_rows(t), y;
	size_t n = epd_frame_words(t);
	u32 *buf = malloc(n * sizeof(*buf));

	epd_frame_template(t, buf);
	for (y = 0; y < rows; y++) {
		const size_t r = (size_t)y * lt;
		bool active = y >= t->fsl && y < (u32)t->fsl + t->fbl + t->height;
		bool clocked = y < (u32)t->fsl + t->fbl + t->height;

		CHECK_EQ(count(buf, r, r + lt, GDOE), active ? lt : 0);
		CHECK_EQ(count(buf, r, r + lt, SDOE), active ? lt : 0);
		CHECK_EQ(count(buf, r, r + lt, GDCLK), clocked ? lt - (lt >> 3) : 0);
	}
	CHECK_EQ(count(buf, 0, n, SDLE), (u32)t->height * t->lsl);
	for (y = 0; y < t->height; y++) {
		size_t d = epd_data_start(t) + (size_t)y * lt;

		CHECK_EQ(count(buf, d, d + t->ldl, SDSP | SDLE), 0);
		CHECK(buf[d - 1] & SDSP);
		CHECK(buf[d + t->ldl] & SDSP);
	}
	free(buf);
}

TEST(frame_encode_packs_two_bits_per_pixel)
{
	const struct epd_timing *t = &fixture_timing;
	u32 lt = epd_lt(t), start = epd_data_start(t), x, y;
	size_t n = epd_frame_words(t), px = (size_t)t->width * t->height, i;
	u32 *tmpl = malloc(n * sizeof(*tmpl)), *buf = malloc(n * sizeof(*buf));
	u8 *drive = malloc(px);

	epd_frame_template(t, tmpl);
	for (i = 0; i < px; i++)
		drive[i] = (u8)(i * 7 + i / 5);
	memset(buf, 0xa5, n * sizeof(*buf));
	epd_frame_encode(t, tmpl, drive, buf);
	for (y = 0; y < t->height; y++)
		for (x = 0; x < t->width; x++) {
			u32 w = buf[start + (size_t)y * lt + x / 8];

			CHECK_EQ(w >> (14 - 2 * (x % 8)) & 3, drive[(size_t)y * t->width + x] & 3);
		}
	for (i = 0; i < n; i++)
		CHECK_EQ(buf[i] & ~0xffffu, tmpl[i]);
	for (i = 0; i < start; i++)
		CHECK_EQ(buf[i], tmpl[i]);
	free(tmpl);
	free(buf);
	free(drive);
}

TEST(frame_template_explicit_gate_clock)
{
	struct epd_timing t = fixture_timing;
	u32 lt, low, y, x;
	u32 *buf;

	t.lgonl = 3;
	t.gdck_sta = 1;
	lt = epd_lt(&t);
	low = (u32)(t.lsl - (t.lbl >> 1)) + t.lbl * 2u + t.gdck_sta;
	buf = malloc(epd_frame_words(&t) * sizeof(*buf));
	epd_frame_template(&t, buf);
	for (y = 0; y < (u32)t.fsl + t.fbl + t.height; y++)
		for (x = 0; x < lt; x++)
			CHECK_EQ(!!(buf[(size_t)y * lt + x] & GDCLK), x >= low && x < low + t.lgonl);
	free(buf);
}

TEST(frame_template_source_start_pulse_over_data)
{
	struct epd_timing t = fixture_timing;
	u32 lt, y, x;
	u32 *buf;

	t.sdsp_pulse_enable = 1;
	lt = epd_lt(&t);
	buf = malloc(epd_frame_words(&t) * sizeof(*buf));
	epd_frame_template(&t, buf);
	for (y = 0; y < t.height; y++) {
		size_t d = epd_data_start(&t) + (size_t)y * lt;

		CHECK_EQ(buf[d] & SDSP, 0);
		for (x = 1; x < t.ldl; x++)
			CHECK(buf[d + x] & SDSP);
	}
	free(buf);
}

TEST(frame_copy_keeps_words_or_swaps_the_data_bytes)
{
	const struct epd_timing *t = &fixture_timing;
	u32 lt = epd_lt(t), rows = epd_rows(t), x, y;
	size_t n = epd_frame_words(t), pitch = (size_t)lt * 4 + 12, i;
	u32 *src = calloc(n, sizeof(*src));
	u32 *dst = malloc(rows * pitch);
	unsigned int seed = 3;
	int swap;

	for (i = 0; i < n; i++)
		src[i] = test_rand(&seed) | test_rand(&seed) << 24;
	for (swap = 0; swap < 2; swap++) {
		memset(dst, 0x5a, rows * pitch);
		epd_frame_copy(t, src, dst, pitch, swap);
		for (y = 0; y < rows; y++) {
			const u32 *d = dst + y * (pitch / 4);

			for (x = 0; x < lt; x++) {
				u32 s = src[y * lt + x];
				u32 want = swap ? (s & 0xffff0000u) | (s & 0xff) << 8 | (s >> 8 & 0xff) : s;

				CHECK_EQ(d[x], want);
			}
			for (i = lt; i < pitch / 4; i++)
				CHECK_EQ(d[i], 0x5a5a5a5a);
		}
	}
	free(src);
	free(dst);
}
