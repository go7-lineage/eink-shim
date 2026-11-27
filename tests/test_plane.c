// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>
#include <string.h>

#include "epd_convert.h"
#include "epd_plane.h"
#include "fixture.h"
#include "test.h"

#define W	48
#define H	6
#define GC16	2
#define A2	4
#define Y1	0x2000

struct plane_rig {
	struct epd_plane pl;
	struct epd_engine *e;
	u32 *frame;
};

static void plane_rig_init(struct plane_rig *r)
{
	struct epd_engine_cfg c = { .timing = &fixture_timing, .temp_c = 10, .tables = { .a2_clean_mode = 2 } };
	const struct wbf_blob *b = fixture_plain_wbf();
	size_t n = epd_frame_words(&fixture_timing);
	int err = 0;

	memset(r, 0, sizeof(*r));
	CHECK_EQ(epd_plane_init(&r->pl, W, H, true), 0);
	r->frame = malloc(n * sizeof(u32));
	epd_frame_template(&fixture_timing, r->frame);
	r->e = epd_engine_create(&c, b->data, b->size, &err);
	if (!r->e)
		abort();
}

static void plane_rig_free(struct plane_rig *r)
{
	epd_engine_destroy(r->e);
	epd_plane_release(&r->pl);
	free(r->frame);
}

static void push(struct plane_rig *r, u16 left, u16 top, u16 w, u16 h, int mode, int full, u32 flags, u8 grey)
{
	struct epd_request q = { .top = top, .left = left, .width = w, .height = h, .mode = mode,
				 .update_mode = full, .flags = flags, .once = true };
	u32 y;

	for (y = top; y < (u32)top + h; y++)
		memset(r->pl.in + y * W + left, grey, w);
	epd_plane_push(&r->pl, &q, 1);
}

static int start(struct plane_rig *r, u32 conv)
{
	struct epd_decision d;
	int n = 0, ret = 0;

	while (epd_plane_pop(&r->pl, r->e, conv, &d, &ret)) {
		CHECK_EQ(ret, 0);
		n++;
	}
	return n;
}

static int run(struct plane_rig *r)
{
	int n = 0;

	while (epd_engine_tick(r->e, r->frame) == 1 && n < 10000)
		n++;
	return n;
}

static void count(const struct plane_rig *r, u32 x0, int *black, int *white, int *other)
{
	u32 x, y;

	*black = 0;
	*white = 0;
	*other = 0;
	for (y = 0; y < H; y++)
		for (x = x0; x < W; x++) {
			int level = epd_engine_level(r->e, x, y);

			if (level == 0)
				(*black)++;
			else if (level == 15)
				(*white)++;
			else
				(*other)++;
		}
}

TEST(plane_init_validates_and_starts_white)
{
	struct epd_plane pl;
	size_t i;

	CHECK_EQ(epd_plane_init(&pl, 0, 4, true), -EINVAL);
	CHECK_EQ(epd_plane_init(&pl, 4, 0, true), -EINVAL);
	epd_plane_release(&pl);
	CHECK_EQ(epd_plane_init(&pl, W, H, true), 0);
	for (i = 0; i < W * H; i++)
		CHECK_EQ(pl.grey[i], 0xff);
	CHECK(pl.q.merge);
	epd_plane_release(&pl);
	epd_plane_release(&pl);
}

TEST(plane_push_copies_only_the_update_into_the_image)
{
	struct plane_rig r;
	u32 x, y;

	plane_rig_init(&r);
	memset(r.pl.in, 0x33, W * H);
	push(&r, 8, 1, 16, 2, GC16, 0, 0, 0x55);
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++)
			CHECK_EQ(r.pl.grey[y * W + x], x >= 8 && x < 24 && y >= 1 && y < 3 ? 0x55 : 0xff);
	CHECK_EQ(r.pl.q.n, 1);
	plane_rig_free(&r);
}

TEST(plane_update_takes_its_format_when_it_starts)
{
	struct plane_rig r;
	int black, white, other;

	plane_rig_init(&r);
	push(&r, 0, 0, W, H, GC16, 1, 0, 0xff);
	CHECK_EQ(start(&r, EPD_CONV_Y1_ORDERED), 1);
	push(&r, 0, 0, W, H, A2, 1, Y1, 0x00);
	push(&r, 24, 0, 24, H, GC16, 0, 0, 0x80);
	CHECK_EQ(start(&r, EPD_CONV_Y1_ORDERED), 0);
	CHECK_EQ(r.pl.q.n, 2);
	run(&r);
	CHECK_EQ(start(&r, EPD_CONV_Y1_ORDERED), 1);
	run(&r);
	CHECK_EQ(epd_engine_level(r.e, 5, 2), 0);
	count(&r, 24, &black, &white, &other);
	CHECK(black > 24 * H / 4);
	CHECK(white > 24 * H / 4);
	CHECK_EQ(other, 0);
	CHECK_EQ(start(&r, EPD_CONV_Y1_ORDERED), 1);
	run(&r);
	count(&r, 24, &black, &white, &other);
	CHECK_EQ(other, 24 * H);
	CHECK_EQ(epd_engine_level(r.e, 30, 2), 8);
	CHECK_EQ(epd_engine_level(r.e, 5, 2), 0);
	plane_rig_free(&r);
}

TEST(plane_grey_update_that_starts_late_shows_the_latest_greys)
{
	struct plane_rig r;
	int black, white, other;

	plane_rig_init(&r);
	push(&r, 0, 0, W, H, A2, 1, Y1, 0xff);
	CHECK_EQ(start(&r, EPD_CONV_Y1_ORDERED), 1);
	push(&r, 0, 0, W, H, GC16, 1, 0, 0x40);
	push(&r, 0, 0, W, H, A2, 1, Y1, 0x80);
	CHECK_EQ(r.pl.q.n, 2);
	run(&r);
	CHECK_EQ(start(&r, EPD_CONV_Y1_ORDERED), 1);
	run(&r);
	CHECK_EQ(epd_engine_level(r.e, 5, 2), 8);
	CHECK_EQ(epd_engine_level(r.e, W - 1, H - 1), 8);
	CHECK_EQ(start(&r, EPD_CONV_Y1_ORDERED), 1);
	run(&r);
	count(&r, 0, &black, &white, &other);
	CHECK(black > W * H / 4);
	CHECK(white > W * H / 4);
	CHECK_EQ(other, 0);
	plane_rig_free(&r);
}

TEST(plane_without_the_ordered_option_uses_the_stock_dither)
{
	static u8 want[W * H];
	static int err[3 * W + 12];
	struct plane_rig r;
	u32 x, y;

	plane_rig_init(&r);
	push(&r, 0, 0, W, H, GC16, 1, 0, 0x00);
	CHECK_EQ(start(&r, 0), 1);
	run(&r);
	push(&r, 0, 0, W, H, A2, 1, Y1, 0xc0);
	CHECK_EQ(start(&r, 0), 1);
	run(&r);
	memset(want, 0xc0, sizeof(want));
	epd_dither_y1(want, W, H, W, err, 0xff, false);
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++)
			CHECK_EQ(epd_engine_level(r.e, x, y) >= 8, want[y * W + x] != 0);
	plane_rig_free(&r);
}

TEST(plane_strips_of_one_frame_share_the_decision)
{
	struct epd_request q[3];
	struct epd_decision d;
	struct plane_rig r;
	int i, ret;

	plane_rig_init(&r);
	push(&r, 0, 0, W, H, GC16, 1, 0, 0xff);
	CHECK_EQ(start(&r, 0), 1);
	for (i = 0; i < 3; i++)
		q[i] = (struct epd_request){ .top = (u16)(2 * i), .left = (u16)(8 * i), .width = 8, .height = 1,
					     .mode = A2, .flags = Y1, .marker = 9 };
	epd_plane_push(&r.pl, q, 3);
	CHECK_EQ(r.pl.q.n, 3);
	run(&r);
	for (i = 0; i < 3; i++) {
		CHECK(epd_plane_pop(&r.pl, r.e, 0, &d, &ret));
		CHECK_EQ(d.upd.mode, 1);
		CHECK_EQ(d.upd.full, true);
		CHECK_EQ(d.upd.left, 8 * i);
		CHECK_EQ(d.barrier, i == 0);
	}
	plane_rig_free(&r);
}

TEST(plane_stock_dither_warms_up_before_the_update)
{
	static u8 want[W * H];
	static int err[3 * W + 12];
	struct plane_rig r;
	u32 x, y;

	plane_rig_init(&r);
	push(&r, 0, 0, W, H, GC16, 1, 0, 0x00);
	CHECK_EQ(start(&r, 0), 1);
	run(&r);
	push(&r, 0, 0, W, H, GC16, 0, 0, 0x80);
	CHECK_EQ(start(&r, 0), 1);
	run(&r);
	push(&r, 24, 4, 16, 2, A2, 1, Y1, 0x80);
	CHECK_EQ(start(&r, 0), 1);
	run(&r);
	memset(want, 0x80, sizeof(want));
	epd_dither_y1(want + 8, W - 8 - 8, H, W, err, 0xff, false);
	CHECK(want[4 * W + 24] != want[4 * W + 26] || want[4 * W + 25] != want[4 * W + 27]);
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++) {
			bool in = x >= 24 && x < 40 && y >= 4;

			if (in)
				CHECK_EQ(epd_engine_level(r.e, x, y) >= 8, want[y * W + x] != 0);
			else
				CHECK_EQ(epd_engine_level(r.e, x, y), 8);
		}
	CHECK_EQ(EPD_PLANE_WARM, 16);
	plane_rig_free(&r);
}

TEST(plane_ordered_dither_needs_no_warm_up)
{
	static u8 want[W * H];
	struct plane_rig r;
	u32 x, y;

	plane_rig_init(&r);
	push(&r, 0, 0, W, H, GC16, 1, 0, 0x00);
	CHECK_EQ(start(&r, EPD_CONV_Y1_ORDERED), 1);
	run(&r);
	push(&r, 24, 4, 16, 2, A2, 1, Y1, 0x80);
	CHECK_EQ(start(&r, EPD_CONV_Y1_ORDERED), 1);
	run(&r);
	memset(want, 0x80, sizeof(want));
	epd_dither_y1_ordered(want, W, H, W, 0, 0);
	for (y = 4; y < H; y++)
		for (x = 24; x < 40; x++)
			CHECK_EQ(epd_engine_level(r.e, x, y) >= 8, want[y * W + x] != 0);
	plane_rig_free(&r);
}
