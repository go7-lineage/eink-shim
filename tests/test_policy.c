// SPDX-License-Identifier: GPL-2.0-only
#include <string.h>

#include "epd_policy.h"
#include "test.h"

#define DU	1
#define GC16	2
#define A2	4
#define REGAL	5
#define AUTO	0xff

static struct epd_decision apply(struct epd_policy *p, int mode, int update_mode, u32 flags)
{
	struct epd_request r = { .top = 1, .left = 2, .width = 3, .height = 4, .mode = mode,
				 .update_mode = update_mode, .marker = 77, .flags = flags };
	struct epd_decision d;

	memset(&d, 0xee, sizeof(d));
	epd_policy_apply(p, &r, &d);
	CHECK_EQ(d.upd.top, 1);
	CHECK_EQ(d.upd.left, 2);
	CHECK_EQ(d.upd.width, 3);
	CHECK_EQ(d.upd.height, 4);
	CHECK_EQ(d.upd.marker, 77);
	CHECK_EQ(d.flags, flags);
	return d;
}

#define EXPECT(d, m, f, b)			\
	do {					\
		struct epd_decision d_ = (d);	\
						\
		CHECK_EQ(d_.upd.mode, m);	\
		CHECK_EQ(d_.upd.full, f);	\
		CHECK_EQ(d_.barrier, b);	\
	} while (0)

TEST(policy_first_update_passes_through)
{
	struct epd_policy p;

	epd_policy_init(&p, true);
	EXPECT(apply(&p, GC16, 0, 0), GC16, false, false);
	epd_policy_init(&p, true);
	EXPECT(apply(&p, GC16, 1, 0), GC16, true, false);
	epd_policy_init(&p, true);
	EXPECT(apply(&p, AUTO, 0, 0), GC16, false, false);
	epd_policy_init(&p, true);
	EXPECT(apply(&p, A2, 0, 0), A2, true, false);
	epd_policy_init(&p, true);
	EXPECT(apply(&p, DU, 0, 0x8000), DU, true, false);
}

TEST(policy_leaving_du_is_a_full_barrier)
{
	struct epd_policy p;

	epd_policy_init(&p, false);
	apply(&p, DU, 0, 0);
	EXPECT(apply(&p, DU, 0, 0), DU, false, false);
	EXPECT(apply(&p, GC16, 0, 8), GC16, true, true);
	EXPECT(apply(&p, GC16, 0, 8), GC16, false, false);
}

TEST(policy_gc16_without_flag_8_is_fenced_from_du_and_a2)
{
	struct epd_policy p;

	epd_policy_init(&p, false);
	apply(&p, GC16, 0, 0);
	EXPECT(apply(&p, A2, 0, 0), A2, true, true);
	EXPECT(apply(&p, GC16, 0, 0), GC16, true, true);
	EXPECT(apply(&p, DU, 0, 0), DU, false, true);

	epd_policy_init(&p, false);
	apply(&p, GC16, 0, 8);
	EXPECT(apply(&p, A2, 0, 0), A2, true, false);
	EXPECT(apply(&p, GC16, 0, 8), GC16, true, false);
}

TEST(policy_auto_counts_as_gc16)
{
	struct epd_policy p;

	epd_policy_init(&p, false);
	apply(&p, AUTO, 0, 0);
	EXPECT(apply(&p, DU, 0, 0), DU, false, true);
	EXPECT(apply(&p, AUTO, 0, 0), GC16, true, true);
}

TEST(policy_leaving_regal_is_full)
{
	struct epd_policy p;

	epd_policy_init(&p, false);
	apply(&p, REGAL, 0, 0);
	EXPECT(apply(&p, 16, 0, 0), 16, false, false);
	EXPECT(apply(&p, GC16, 0, 8), GC16, true, false);
}

TEST(policy_a2_entry_is_du_when_cleaning)
{
	struct epd_policy p;

	epd_policy_init(&p, true);
	apply(&p, GC16, 0, 8);
	EXPECT(apply(&p, A2, 0, 0), DU, true, false);
	EXPECT(apply(&p, A2, 0, 0), A2, true, false);
	EXPECT(apply(&p, GC16, 0, 8), GC16, true, true);

	epd_policy_init(&p, false);
	apply(&p, GC16, 0, 8);
	EXPECT(apply(&p, A2, 0, 0), A2, true, false);
	EXPECT(apply(&p, GC16, 0, 8), GC16, true, false);
}

TEST(policy_a2_repeats_unless_asked_once)
{
	struct epd_request r = { .width = 8, .height = 8, .mode = A2 };
	struct epd_decision d[2];
	struct epd_policy p;

	epd_policy_init(&p, true);
	CHECK_EQ(apply(&p, A2, 0, 0).upd.repeat, true);
	CHECK_EQ(apply(&p, GC16, 0, 0).upd.repeat, false);
	CHECK_EQ(apply(&p, A2, 0, 0).upd.repeat, false);
	CHECK_EQ(apply(&p, A2, 0, 0).upd.repeat, true);
	r.once = true;
	epd_policy_apply(&p, &r, &d[0]);
	CHECK_EQ(d[0].upd.mode, A2);
	CHECK_EQ(d[0].upd.repeat, false);
	epd_policy_apply_group(&p, &r, 1, d);
	CHECK_EQ(d[0].upd.repeat, false);
	r.once = false;
	epd_policy_apply_group(&p, &r, 1, d);
	CHECK_EQ(d[0].upd.repeat, true);
	p.a2_once = true;
	CHECK_EQ(apply(&p, A2, 0, 0).upd.repeat, false);
	p.a2_once = false;
	CHECK_EQ(apply(&p, A2, 0, 0).upd.repeat, true);
}

TEST(policy_no_clean_keeps_the_update_as_asked_after_a_fast_one)
{
	static const int fast[] = { A2, DU, REGAL };
	struct epd_request r = { .width = 8, .height = 8, .mode = GC16, .no_clean = true };
	struct epd_decision d;
	struct epd_policy p;
	size_t i;

	for (i = 0; i < ARRAY_LEN(fast); i++) {
		epd_policy_init(&p, true);
		apply(&p, fast[i], 0, 0);
		epd_policy_apply(&p, &r, &d);
		CHECK_EQ(d.upd.full, false);
		CHECK_EQ(d.barrier, fast[i] != REGAL);

		epd_policy_init(&p, true);
		apply(&p, fast[i], 0, 0);
		r.update_mode = 1;
		epd_policy_apply(&p, &r, &d);
		CHECK_EQ(d.upd.full, true);
		r.update_mode = 0;

		epd_policy_init(&p, true);
		apply(&p, fast[i], 0, 0);
		EXPECT(apply(&p, GC16, 0, 0), GC16, true, fast[i] != REGAL);
	}
	epd_policy_init(&p, true);
	r.mode = A2;
	epd_policy_apply(&p, &r, &d);
	CHECK_EQ(d.upd.full, true);
}

static void apply_group(struct epd_policy *p, int mode, int update_mode, struct epd_decision *d)
{
	struct epd_request r[3];
	int i;

	for (i = 0; i < 3; i++)
		r[i] = (struct epd_request){ .top = (u16)(10 * i), .left = (u16)i, .width = 8, .height = 2,
					     .mode = mode, .update_mode = update_mode, .marker = 50, .flags = 0 };
	memset(d, 0xee, 3 * sizeof(*d));
	epd_policy_apply_group(p, r, 3, d);
	for (i = 0; i < 3; i++) {
		CHECK_EQ(d[i].upd.top, 10 * i);
		CHECK_EQ(d[i].upd.left, i);
		CHECK_EQ(d[i].upd.width, 8);
		CHECK_EQ(d[i].upd.height, 2);
		CHECK_EQ(d[i].upd.marker, 50);
		CHECK_EQ(d[i].flags, 0);
	}
}

TEST(policy_group_after_a2_is_full_everywhere)
{
	struct epd_decision d[3];
	struct epd_policy p;
	int i;

	epd_policy_init(&p, false);
	apply(&p, A2, 0, 0);
	apply_group(&p, AUTO, 0, d);
	for (i = 0; i < 3; i++)
		EXPECT(d[i], GC16, true, false);
	apply_group(&p, AUTO, 0, d);
	for (i = 0; i < 3; i++)
		EXPECT(d[i], GC16, false, false);
}

TEST(policy_group_enters_a2_as_du_everywhere)
{
	struct epd_decision d[3];
	struct epd_policy p;
	int i;

	epd_policy_init(&p, true);
	apply(&p, GC16, 0, 8);
	apply_group(&p, A2, 0, d);
	for (i = 0; i < 3; i++)
		EXPECT(d[i], DU, true, false);
	apply_group(&p, A2, 0, d);
	for (i = 0; i < 3; i++)
		EXPECT(d[i], A2, true, false);
}

TEST(policy_group_barrier_is_only_on_the_first)
{
	struct epd_decision d[3];
	struct epd_policy p;

	epd_policy_init(&p, false);
	apply(&p, DU, 0, 0);
	apply_group(&p, AUTO, 0, d);
	EXPECT(d[0], GC16, true, true);
	EXPECT(d[1], GC16, true, false);
	EXPECT(d[2], GC16, true, false);
}

static struct epd_decision dec(u16 left, u16 top, u16 w, u16 h, int mode, bool full, u32 marker)
{
	struct epd_decision d = { .upd = { .top = top, .left = left, .width = w, .height = h, .mode = mode,
					   .full = full, .marker = marker } };

	return d;
}

TEST(queue_is_fifo)
{
	struct epd_queue q;
	struct epd_decision d, out;
	u32 i;

	epd_queue_init(&q);
	CHECK(!epd_queue_pop(&q, false, &out));
	for (i = 0; i < 40; i++) {
		d = dec(0, 0, 8, 8, GC16, false, i);
		epd_queue_push(&q, &d);
		d = dec(0, 0, 8, 8, GC16, false, 100 + i);
		epd_queue_push(&q, &d);
		CHECK(epd_queue_pop(&q, true, &out));
		CHECK_EQ(out.upd.marker, i);
		CHECK(epd_queue_pop(&q, true, &out));
		CHECK_EQ(out.upd.marker, 100 + i);
	}
	CHECK(!epd_queue_pop(&q, false, &out));
	CHECK_EQ(q.coalesced, 0);
}

TEST(queue_barrier_waits_for_the_engine)
{
	struct epd_queue q;
	struct epd_decision d = dec(0, 0, 8, 8, GC16, false, 1), out;

	epd_queue_init(&q);
	d.barrier = true;
	epd_queue_push(&q, &d);
	d = dec(0, 0, 8, 8, GC16, false, 2);
	epd_queue_push(&q, &d);
	CHECK(!epd_queue_pop(&q, true, &out));
	CHECK_EQ(q.n, 2);
	CHECK(epd_queue_pop(&q, false, &out));
	CHECK_EQ(out.upd.marker, 1);
	CHECK(epd_queue_pop(&q, true, &out));
	CHECK_EQ(out.upd.marker, 2);
}

TEST(queue_keeps_overlapping_updates_apart)
{
	struct epd_queue q;
	struct epd_decision d, out;

	epd_queue_init(&q);
	q.merge = true;
	d = dec(10, 10, 20, 20, DU, false, 1);
	epd_queue_push(&q, &d);
	d = dec(25, 5, 10, 10, DU, false, 2);
	epd_queue_push(&q, &d);
	CHECK_EQ(q.n, 2);
	CHECK_EQ(q.coalesced, 0);
	CHECK(epd_queue_pop(&q, false, &out));
	CHECK_EQ(out.upd.left, 10);
	CHECK_EQ(out.upd.top, 10);
	CHECK_EQ(out.upd.width, 20);
	CHECK_EQ(out.upd.height, 20);
	CHECK_EQ(out.upd.marker, 1);
}

TEST(queue_absorbs_an_update_inside_a_queued_one)
{
	struct epd_queue q;
	struct epd_decision d, out;

	epd_queue_init(&q);
	q.merge = true;
	d = dec(10, 10, 20, 20, GC16, false, 1);
	epd_queue_push(&q, &d);
	d = dec(40, 40, 4, 4, GC16, false, 2);
	epd_queue_push(&q, &d);
	d = dec(12, 12, 8, 8, GC16, false, 3);
	epd_queue_push(&q, &d);
	CHECK_EQ(q.n, 2);
	CHECK_EQ(q.coalesced, 1);
	d = dec(36, 36, 12, 12, GC16, false, 4);
	epd_queue_push(&q, &d);
	CHECK_EQ(q.n, 2);
	CHECK_EQ(q.coalesced, 2);
	CHECK(epd_queue_pop(&q, false, &out));
	CHECK_EQ(out.upd.left, 10);
	CHECK_EQ(out.upd.width, 20);
	CHECK_EQ(out.upd.marker, 3);
	CHECK(epd_queue_pop(&q, false, &out));
	CHECK_EQ(out.upd.left, 36);
	CHECK_EQ(out.upd.top, 36);
	CHECK_EQ(out.upd.width, 12);
	CHECK_EQ(out.upd.height, 12);
	CHECK_EQ(out.upd.marker, 4);
}

TEST(queue_joins_updates_that_tile)
{
	static const u16 at[4][4] = { { 10, 30, 20, 5 }, { 10, 25, 20, 9 }, { 30, 10, 3, 20 }, { 4, 10, 6, 20 } };
	static const u16 want[4][4] = { { 10, 10, 20, 25 }, { 10, 10, 20, 24 }, { 10, 10, 23, 20 }, { 4, 10, 26, 20 } };
	struct epd_queue q;
	struct epd_decision d, out;
	u32 i;

	for (i = 0; i < 4; i++) {
		epd_queue_init(&q);
		q.merge = true;
		d = dec(10, 10, 20, 20, A2, true, 1);
		epd_queue_push(&q, &d);
		d = dec(at[i][0], at[i][1], at[i][2], at[i][3], A2, true, 2);
		epd_queue_push(&q, &d);
		CHECK_EQ(q.n, 1);
		CHECK(epd_queue_pop(&q, false, &out));
		CHECK_EQ(out.upd.left, want[i][0]);
		CHECK_EQ(out.upd.top, want[i][1]);
		CHECK_EQ(out.upd.width, want[i][2]);
		CHECK_EQ(out.upd.height, want[i][3]);
	}
}

TEST(queue_only_joins_the_same_kind)
{
	struct epd_queue q;
	struct epd_decision d;

	epd_queue_init(&q);
	q.merge = true;
	d = dec(200, 100, 4, 4, DU, false, 3);
	epd_queue_push(&q, &d);
	d = dec(100, 100, 4, 4, GC16, false, 4);
	epd_queue_push(&q, &d);
	d = dec(100, 100, 4, 4, GC16, true, 5);
	epd_queue_push(&q, &d);
	d = dec(100, 100, 4, 4, GC16, true, 6);
	d.flags = 8;
	epd_queue_push(&q, &d);
	d = dec(100, 100, 4, 4, GC16, true, 7);
	d.flags = 8;
	d.barrier = true;
	epd_queue_push(&q, &d);
	d = dec(100, 100, 4, 4, A2, true, 8);
	epd_queue_push(&q, &d);
	d.upd.repeat = true;
	epd_queue_push(&q, &d);
	CHECK_EQ(q.n, 7);
	d = dec(100, 100, 4, 4, DU, false, 9);
	epd_queue_push(&q, &d);
	CHECK_EQ(q.n, 8);
	CHECK_EQ(q.coalesced, 0);
}

TEST(queue_grey_update_replaces_queued_fast_updates_it_covers)
{
	struct epd_queue q;
	struct epd_decision d, out;

	epd_queue_init(&q);
	q.merge = true;
	d = dec(0, 0, 8, 8, GC16, false, 1);
	epd_queue_push(&q, &d);
	d = dec(10, 10, 20, 20, DU, true, 2);
	d.barrier = true;
	epd_queue_push(&q, &d);
	d = dec(10, 40, 20, 20, DU, true, 3);
	epd_queue_push(&q, &d);
	d = dec(12, 12, 4, 4, A2, true, 4);
	epd_queue_push(&q, &d);
	d = dec(8, 8, 30, 30, GC16, true, 5);
	epd_queue_push(&q, &d);
	CHECK_EQ(q.n, 3);
	CHECK_EQ(q.coalesced, 2);
	CHECK(epd_queue_pop(&q, true, &out));
	CHECK_EQ(out.upd.marker, 1);
	CHECK(!epd_queue_pop(&q, true, &out));
	CHECK(epd_queue_pop(&q, false, &out));
	CHECK_EQ(out.upd.marker, 3);
	CHECK(epd_queue_pop(&q, true, &out));
	CHECK_EQ(out.upd.marker, 5);
}

TEST(queue_grey_update_takes_over_the_barrier_of_the_last_one_it_replaces)
{
	struct epd_queue q;
	struct epd_decision d, out;

	epd_queue_init(&q);
	q.merge = true;
	d = dec(10, 10, 20, 20, DU, true, 2);
	d.barrier = true;
	epd_queue_push(&q, &d);
	d = dec(0, 0, 40, 40, 5, true, 3);
	epd_queue_push(&q, &d);
	CHECK_EQ(q.n, 1);
	CHECK(!epd_queue_pop(&q, true, &out));
	CHECK(epd_queue_pop(&q, false, &out));
	CHECK_EQ(out.upd.marker, 3);

	d = dec(10, 10, 20, 20, A2, true, 4);
	d.barrier = true;
	epd_queue_push(&q, &d);
	d = dec(10, 10, 20, 20, GC16, false, 5);
	epd_queue_push(&q, &d);
	CHECK_EQ(q.n, 1);
	CHECK(!epd_queue_pop(&q, true, &out));
	CHECK(epd_queue_pop(&q, false, &out));
	CHECK_EQ(out.upd.marker, 5);
	CHECK_EQ(out.upd.full, false);
}

TEST(queue_keeps_queued_updates_a_new_one_does_not_replace)
{
	struct epd_queue q;
	struct epd_decision d;

	epd_queue_init(&q);
	q.merge = true;
	d = dec(10, 10, 20, 20, A2, true, 1);
	epd_queue_push(&q, &d);
	d = dec(11, 0, 40, 40, GC16, false, 2);
	epd_queue_push(&q, &d);
	d = dec(0, 0, 40, 40, DU, true, 3);
	epd_queue_push(&q, &d);
	d = dec(12, 0, 40, 40, GC16, true, 4);
	epd_queue_push(&q, &d);
	CHECK_EQ(q.n, 4);
	d = dec(0, 0, 60, 60, GC16, true, 5);
	epd_queue_push(&q, &d);
	CHECK_EQ(q.n, 2);

	epd_queue_init(&q);
	d = dec(10, 10, 20, 20, A2, true, 1);
	epd_queue_push(&q, &d);
	d = dec(0, 0, 40, 40, GC16, true, 2);
	epd_queue_push(&q, &d);
	CHECK_EQ(q.n, 2);
}

TEST(queue_does_not_merge_unless_enabled)
{
	struct epd_queue q;
	struct epd_decision d = dec(10, 10, 20, 20, DU, false, 1);

	epd_queue_init(&q);
	epd_queue_push(&q, &d);
	epd_queue_push(&q, &d);
	CHECK_EQ(q.n, 2);
}

TEST(queue_overflow_folds_into_the_tail)
{
	struct epd_queue q;
	struct epd_decision d, out;
	u32 i;

	epd_queue_init(&q);
	for (i = 0; i < EPD_QMAX; i++) {
		d = dec((u16)(10 * i), 0, 5, 5, DU, false, i);
		epd_queue_push(&q, &d);
	}
	d = dec(500, 40, 10, 10, GC16, true, 99);
	d.flags = 8;
	d.barrier = true;
	epd_queue_push(&q, &d);
	CHECK_EQ(q.n, EPD_QMAX);
	CHECK_EQ(q.coalesced, 1);
	for (i = 0; i < EPD_QMAX - 1; i++) {
		CHECK(epd_queue_pop(&q, false, &out));
		CHECK_EQ(out.upd.marker, i);
	}
	CHECK(!epd_queue_pop(&q, true, &out));
	CHECK(epd_queue_pop(&q, false, &out));
	CHECK_EQ(out.upd.left, 150);
	CHECK_EQ(out.upd.top, 0);
	CHECK_EQ(out.upd.width, 360);
	CHECK_EQ(out.upd.height, 50);
	CHECK_EQ(out.upd.mode, GC16);
	CHECK_EQ(out.upd.full, true);
	CHECK_EQ(out.upd.marker, 99);
	CHECK_EQ(out.flags, 8);
	CHECK(!epd_queue_pop(&q, false, &out));
}

TEST(queue_does_not_merge_rectangles_that_only_touch)
{
	static const u16 at[4][2] = { { 30, 10 }, { 0, 10 }, { 10, 30 }, { 10, 0 } };
	struct epd_queue q;
	struct epd_decision d;
	u32 i;

	for (i = 0; i < 4; i++) {
		epd_queue_init(&q);
		q.merge = true;
		d = dec(10, 10, 20, 20, DU, false, 1);
		epd_queue_push(&q, &d);
		d = dec(at[i][0], at[i][1], 10, 10, DU, false, 2);
		epd_queue_push(&q, &d);
		CHECK_EQ(q.n, 2);
	}
}

static bool clip(s32 left, s32 top, s32 width, s32 height, struct epd_request *r)
{
	memset(r, 0xee, sizeof(*r));
	return epd_request_clip(r, left, top, width, height, 1264, 1680);
}

#define EXPECT_RECT(r, x, y, w, h)		\
	do {					\
		CHECK_EQ((r)->left, x);		\
		CHECK_EQ((r)->top, y);		\
		CHECK_EQ((r)->width, w);	\
		CHECK_EQ((r)->height, h);	\
	} while (0)

TEST(request_clip_inside_is_unchanged)
{
	struct epd_request r;

	CHECK(clip(10, 20, 30, 40, &r));
	EXPECT_RECT(&r, 10, 20, 30, 40);
	CHECK(clip(0, 0, 1264, 1680, &r));
	EXPECT_RECT(&r, 0, 0, 1264, 1680);
	CHECK(clip(1263, 1679, 1, 1, &r));
	EXPECT_RECT(&r, 1263, 1679, 1, 1);
	CHECK_EQ(r.mode, (int)0xeeeeeeee);
}

TEST(request_clip_cuts_at_the_far_edges)
{
	struct epd_request r;

	CHECK(clip(1200, 1600, 500, 500, &r));
	EXPECT_RECT(&r, 1200, 1600, 64, 80);
	CHECK(clip(0, 0, 0x7fffffff, 0x7fffffff, &r));
	EXPECT_RECT(&r, 0, 0, 1264, 1680);
}

TEST(request_clip_cuts_at_the_near_edges)
{
	struct epd_request r;

	CHECK(clip(-10, -20, 100, 200, &r));
	EXPECT_RECT(&r, 0, 0, 90, 180);
	CHECK(!clip(-0x7fffffff - 1, 5, 0x7fffffff, 1, &r));
	CHECK(clip(-0x7fffffff - 1, 5, -1, 1, &r) == false);
	CHECK(clip(-1, -1, 0x7fffffff, 0x7fffffff, &r));
	EXPECT_RECT(&r, 0, 0, 1264, 1680);
}

TEST(request_clip_rejects_empty_and_outside)
{
	struct epd_request r;

	CHECK(!clip(0, 0, 0, 10, &r));
	CHECK(!clip(0, 0, 10, 0, &r));
	CHECK(!clip(0, 0, -5, 10, &r));
	CHECK(!clip(1264, 0, 10, 10, &r));
	CHECK(!clip(0, 1680, 10, 10, &r));
	CHECK(!clip(-10, 0, 10, 10, &r));
	CHECK(!clip(0, -10, 10, 10, &r));
	CHECK(!clip(0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff, &r));
}
