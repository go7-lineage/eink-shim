// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>
#include <string.h>

#include "fixture.h"
#include "test.h"

#define W	48
#define H	6
#define GC16	2
#define DU	1
#define A2	4
#define REGAL	5

struct rig {
	struct epd_engine *e;
	u32 *frame, *tmpl;
	u8 grey[W * H];
	u32 done[512];
	int ndone;
};

static void on_done(void *ctx, u32 marker)
{
	struct rig *r = ctx;

	if (r->ndone < (int)ARRAY_LEN(r->done))
		r->done[r->ndone] = marker;
	r->ndone++;
}

static struct epd_engine_cfg base_cfg(struct rig *r)
{
	struct epd_engine_cfg c = {
		.timing = &fixture_timing,
		.temp_c = 10,
		.marker_done = on_done,
		.ctx = r,
	};

	return c;
}

static void rig_init_cfg(struct rig *r, const struct epd_engine_cfg *cfg)
{
	const struct wbf_blob *b = fixture_wbf();
	size_t n = epd_frame_words(&fixture_timing);
	int err = 0;

	memset(r, 0, sizeof(*r));
	r->frame = malloc(n * sizeof(u32));
	r->tmpl = malloc(n * sizeof(u32));
	epd_frame_template(&fixture_timing, r->tmpl);
	memcpy(r->frame, r->tmpl, n * sizeof(u32));
	r->e = epd_engine_create(cfg, b->data, b->size, &err);
	if (!r->e)
		abort();
	CHECK_EQ(err, 0);
}

static void rig_init(struct rig *r)
{
	struct epd_engine_cfg c = base_cfg(r);

	rig_init_cfg(r, &c);
}

static void rig_free(struct rig *r)
{
	epd_engine_destroy(r->e);
	free(r->frame);
	free(r->tmpl);
}

static u8 drive_at(const struct rig *r, u32 x, u32 y)
{
	const struct epd_timing *t = &fixture_timing;
	u32 w = r->frame[epd_data_start(t) + (size_t)y * epd_lt(t) + x / 8];

	return (u8)(w >> (14 - 2 * (x % 8)) & 3);
}

static int submit(struct rig *r, u16 left, u16 top, u16 w, u16 h, int mode, bool full, u32 marker)
{
	struct epd_update u = { .top = top, .left = left, .width = w, .height = h, .mode = mode,
				.full = full, .marker = marker };

	return epd_engine_submit(r->e, &u, r->grey, W);
}

static int run(struct rig *r)
{
	int n = 0;

	while (epd_engine_tick(r->e, r->frame) == 1 && n < 10000)
		n++;
	return n;
}

static void fill(struct rig *r, u8 level)
{
	memset(r->grey, level << 4 | 0x7, sizeof(r->grey));
}

static void pattern(struct rig *r, u32 seed)
{
	u32 i;

	for (i = 0; i < W * H; i++)
		r->grey[i] = (u8)((i * 5 + i / W * 3 + seed) % 16 << 4);
}

static void settle(struct rig *r, u8 level)
{
	fill(r, level);
	CHECK_EQ(submit(r, 0, 0, W, H, GC16, true, 9000), 0);
	run(r);
	r->ndone = 0;
}

TEST(engine_create_rejects_unusable_input)
{
	static const u8 bounds[] = { 0, 50 };
	struct wbf_spec s = fixture_spec;
	struct epd_timing odd = fixture_timing;
	const struct wbf_blob *b = fixture_wbf();
	struct epd_engine_cfg c = { .timing = &fixture_timing };
	struct wbf_blob b16;
	u8 junk[64] = { 0 };
	int err = 0;

	CHECK(!epd_engine_create(&c, junk, sizeof(junk), &err));
	CHECK_EQ(err, -EINVAL);

	s.lut_fmt = 0;
	s.n_temps = 1;
	s.temp_bounds = bounds;
	wbf_build(&s, &b16);
	err = 0;
	CHECK(!epd_engine_create(&c, b16.data, b16.size, &err));
	CHECK_EQ(err, -EINVAL);
	free(b16.data);

	odd.width = 44;
	c.timing = &odd;
	err = 0;
	CHECK(!epd_engine_create(&c, b->data, b->size, &err));
	CHECK_EQ(err, -EINVAL);
}

TEST(engine_full_update_drives_every_pixel_from_black)
{
	struct rig r;
	u32 x, y;
	int f, n = (int)fixture_frames(2, 0);

	rig_init(&r);
	pattern(&r, 1);
	CHECK(!epd_engine_busy(r.e));
	CHECK_EQ(epd_engine_tick(r.e, r.frame), 0);
	CHECK_EQ(submit(&r, 0, 0, W, H, GC16, true, 7), 0);
	CHECK(epd_engine_busy(r.e));
	for (f = 0; f < n; f++) {
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
		for (y = 0; y < H; y++)
			for (x = 0; x < W; x++)
				CHECK_EQ(drive_at(&r, x, y),
					 fixture_drive(2, 0, (unsigned int)f, 2u * (r.grey[y * W + x] >> 4), 0));
		CHECK_EQ(r.ndone, f == n - 1);
	}
	CHECK(!epd_engine_busy(r.e));
	CHECK_EQ(epd_engine_tick(r.e, r.frame), 0);
	CHECK_EQ(r.done[0], 7);
	rig_free(&r);
}

TEST(engine_tick_keeps_control_bits_and_blanks_idle_pixels)
{
	struct rig r;
	size_t n = epd_frame_words(&fixture_timing), i;
	u32 x, y;

	rig_init(&r);
	for (i = 0; i < n; i++)
		r.frame[i] |= 0xffff;
	fill(&r, 5);
	CHECK_EQ(submit(&r, 8, 2, 16, 2, GC16, true, 1), 0);
	CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
	for (i = 0; i < n; i++)
		CHECK_EQ(r.frame[i] & ~0xffffu, r.tmpl[i]);
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++) {
			bool in = x >= 8 && x < 24 && y >= 2 && y < 4;

			CHECK_EQ(drive_at(&r, x, y), in ? fixture_drive(2, 0, 0, 10, 0) : 0);
		}
	rig_free(&r);
}

TEST(engine_partial_update_drives_changed_pixels_only)
{
	struct rig r;
	u8 before[W * H];
	u32 x, y;
	int f, n = (int)fixture_frames(1, 0);

	rig_init(&r);
	pattern(&r, 3);
	CHECK_EQ(submit(&r, 0, 0, W, H, GC16, true, 1), 0);
	run(&r);
	memcpy(before, r.grey, sizeof(before));
	for (x = 10; x < 20; x++)
		r.grey[2 * W + x] ^= 0x80;
	r.ndone = 0;
	CHECK_EQ(submit(&r, 0, 0, W, H, DU, false, 2), 0);
	for (f = 0; f < n; f++) {
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
		for (y = 0; y < H; y++)
			for (x = 0; x < W; x++) {
				bool changed = y == 2 && x >= 10 && x < 20;
				u8 want = changed ? fixture_drive(1, 0, (unsigned int)f,
								  2u * (r.grey[y * W + x] >> 4),
								  2u * (before[y * W + x] >> 4)) : 0;

				CHECK_EQ(drive_at(&r, x, y), want);
			}
	}
	CHECK(!epd_engine_busy(r.e));
	CHECK_EQ(r.ndone, 1);
	rig_free(&r);
}

TEST(engine_partial_update_without_change_completes_at_once)
{
	struct rig r;

	rig_init(&r);
	settle(&r, 9);
	CHECK_EQ(submit(&r, 0, 0, W, H, DU, false, 5), 0);
	CHECK(!epd_engine_busy(r.e));
	CHECK_EQ(r.ndone, 1);
	CHECK_EQ(r.done[0], 5);
	rig_free(&r);
}

TEST(engine_full_update_redrives_unchanged_pixels)
{
	struct rig r;

	rig_init(&r);
	settle(&r, 9);
	CHECK_EQ(submit(&r, 0, 0, 8, 1, DU, true, 5), 0);
	CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
	CHECK_EQ(drive_at(&r, 3, 0), fixture_drive(1, 0, 0, 18, 18));
	CHECK_EQ(run(&r), (int)fixture_frames(1, 0) - 1);
	rig_free(&r);
}

TEST(engine_overlapping_update_waits_for_the_pixels)
{
	struct rig r;
	int na = (int)fixture_frames(2, 0), nb = (int)fixture_frames(1, 0), f;

	rig_init(&r);
	settle(&r, 0);
	fill(&r, 12);
	CHECK_EQ(submit(&r, 0, 0, 24, H, GC16, true, 1), 0);
	fill(&r, 4);
	CHECK_EQ(submit(&r, 16, 0, 24, H, DU, true, 2), 0);
	for (f = 0; f < na; f++) {
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
		CHECK_EQ(drive_at(&r, 20, 1), fixture_drive(2, 0, (unsigned int)f, 24, 0));
		CHECK_EQ(drive_at(&r, 30, 1), f < nb ? fixture_drive(1, 0, (unsigned int)f, 8, 0) : 0);
	}
	CHECK_EQ(r.ndone, 1);
	CHECK_EQ(r.done[0], 1);
	for (f = 0; f < nb; f++) {
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
		CHECK_EQ(drive_at(&r, 20, 1), fixture_drive(1, 0, (unsigned int)f, 8, 24));
		CHECK_EQ(drive_at(&r, 30, 1), 0);
		CHECK_EQ(drive_at(&r, 4, 1), 0);
	}
	CHECK(!epd_engine_busy(r.e));
	CHECK_EQ(r.ndone, 2);
	CHECK_EQ(r.done[1], 2);
	rig_free(&r);
}

TEST(engine_disjoint_updates_run_together)
{
	struct rig r;

	rig_init(&r);
	settle(&r, 0);
	fill(&r, 12);
	CHECK_EQ(submit(&r, 0, 0, 16, H, GC16, true, 1), 0);
	CHECK_EQ(submit(&r, 32, 0, 16, H, DU, true, 2), 0);
	CHECK_EQ(run(&r), (int)fixture_frames(2, 0));
	CHECK_EQ(r.ndone, 2);
	CHECK_EQ(r.done[0], 2);
	CHECK_EQ(r.done[1], 1);
	rig_free(&r);
}

TEST(engine_runs_out_of_luts_then_catches_up)
{
	struct rig r;
	u32 i;
	int n = (int)fixture_frames(1, 0);

	rig_init(&r);
	settle(&r, 0);
	fill(&r, 15);
	for (i = 0; i < EPD_NLUTS + 3; i++)
		CHECK_EQ(submit(&r, (u16)(i % W), (u16)(i / W), 1, 1, DU, true, 100 + i), 0);
	CHECK_EQ(run(&r), 2 * n);
	CHECK_EQ(r.ndone, EPD_NLUTS + 3);
	for (i = 0; i < EPD_NLUTS + 3; i++)
		CHECK_EQ(r.done[i], 100 + i);
	rig_free(&r);
}

static int supersede_run(bool on, struct rig *r)
{
	struct epd_engine_cfg c = base_cfg(r);

	c.supersede = on;
	rig_init_cfg(r, &c);
	settle(r, 0);
	fill(r, 12);
	CHECK_EQ(submit(r, 0, 0, W, H, GC16, true, 1), 0);
	fill(r, 4);
	CHECK_EQ(submit(r, 8, 1, 16, 2, DU, true, 2), 0);
	fill(r, 9);
	CHECK_EQ(submit(r, 8, 1, 16, 2, DU, true, 3), 0);
	return r->ndone;
}

TEST(engine_supersede_drops_covered_waiter)
{
	struct rig r;
	int na = (int)fixture_frames(2, 0), nb = (int)fixture_frames(1, 0), f;

	CHECK_EQ(supersede_run(true, &r), 1);
	CHECK_EQ(r.done[0], 2);
	for (f = 0; f < na; f++)
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
	CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
	CHECK_EQ(drive_at(&r, 10, 1), fixture_drive(1, 0, 0, 18, 24));
	CHECK_EQ(run(&r), nb - 1);
	CHECK_EQ(r.ndone, 3);
	CHECK_EQ(r.done[2], 3);
	rig_free(&r);

	CHECK_EQ(supersede_run(false, &r), 0);
	CHECK_EQ(run(&r), na + 2 * nb);
	CHECK_EQ(r.ndone, 3);
	CHECK_EQ(r.done[1], 2);
	CHECK_EQ(r.done[2], 3);
	rig_free(&r);
}

TEST(engine_supersede_trims_partly_covered_waiter)
{
	struct rig r;
	struct epd_engine_cfg c = base_cfg(&r);
	int na = (int)fixture_frames(2, 0), nb = (int)fixture_frames(1, 0), f;

	c.supersede = true;
	rig_init_cfg(&r, &c);
	settle(&r, 0);
	fill(&r, 12);
	CHECK_EQ(submit(&r, 0, 0, W, H, GC16, true, 1), 0);
	fill(&r, 4);
	CHECK_EQ(submit(&r, 8, 1, 16, 2, DU, true, 2), 0);
	fill(&r, 9);
	CHECK_EQ(submit(&r, 16, 1, 16, 2, DU, true, 3), 0);
	CHECK_EQ(r.ndone, 0);
	for (f = 0; f <= na; f++)
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
	CHECK_EQ(drive_at(&r, 10, 1), fixture_drive(1, 0, 0, 8, 24));
	CHECK_EQ(drive_at(&r, 20, 1), fixture_drive(1, 0, 0, 18, 24));
	CHECK_EQ(drive_at(&r, 28, 1), fixture_drive(1, 0, 0, 18, 24));
	CHECK_EQ(run(&r), nb - 1);
	CHECK_EQ(r.ndone, 3);
	rig_free(&r);
}

TEST(engine_supersede_ignores_other_modes)
{
	struct rig r;
	struct epd_engine_cfg c = base_cfg(&r);

	c.supersede = true;
	rig_init_cfg(&r, &c);
	settle(&r, 0);
	fill(&r, 12);
	CHECK_EQ(submit(&r, 0, 0, W, H, GC16, true, 1), 0);
	CHECK_EQ(submit(&r, 8, 1, 16, 2, DU, true, 2), 0);
	CHECK_EQ(submit(&r, 8, 1, 16, 2, A2, true, 3), 0);
	CHECK_EQ(submit(&r, 8, 1, 16, 2, DU, false, 4), 0);
	CHECK_EQ(r.ndone, 0);
	run(&r);
	CHECK_EQ(r.ndone, 4);
	rig_free(&r);
}

TEST(engine_temperature_selects_the_table)
{
	struct rig r;

	rig_init(&r);
	CHECK_EQ(epd_engine_mode_frames(r.e, GC16, true), (int)fixture_frames(2, 0));
	CHECK_EQ(epd_engine_set_temp(r.e, 25), 0);
	CHECK_EQ(epd_engine_mode_frames(r.e, GC16, true), (int)fixture_frames(2, 1));
	fill(&r, 6);
	CHECK_EQ(submit(&r, 0, 0, 8, 1, GC16, true, 1), 0);
	CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
	CHECK_EQ(drive_at(&r, 0, 0), fixture_drive(2, 1, 0, 12, 0));
	CHECK_EQ(epd_engine_set_temp(r.e, 45), 0);
	CHECK_EQ(run(&r), (int)fixture_frames(2, 1) - 1);
	CHECK_EQ(epd_engine_mode_frames(r.e, GC16, true), (int)fixture_frames(2, 2));
	rig_free(&r);
}

TEST(engine_cut_shortens_a2_and_du)
{
	struct rig r;
	struct epd_engine_cfg c = base_cfg(&r);
	int n = (int)fixture_frames(6, 0), f;

	c.tables.fill_zero = true;
	rig_init_cfg(&r, &c);
	CHECK_EQ(epd_engine_get_cut(r.e), 0);
	CHECK_EQ(epd_engine_mode_frames(r.e, A2, true), n);
	CHECK_EQ(epd_engine_set_cut(r.e, -1), -EINVAL);
	CHECK_EQ(epd_engine_set_cut(r.e, 256), -EINVAL);
	CHECK_EQ(epd_engine_set_cut(r.e, 2), 0);
	CHECK_EQ(epd_engine_get_cut(r.e), 2);
	CHECK_EQ(epd_engine_mode_frames(r.e, A2, true), n - 2);
	CHECK_EQ(epd_engine_mode_frames(r.e, DU, true), (int)fixture_frames(1, 0) - 2);
	CHECK_EQ(epd_engine_mode_frames(r.e, GC16, true), (int)fixture_frames(2, 0));
	fill(&r, 15);
	CHECK_EQ(submit(&r, 0, 0, 8, 1, A2, true, 1), 0);
	for (f = 0; f < n - 2; f++) {
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
		CHECK_EQ(drive_at(&r, 0, 0), f == n - 3 ? 0 : fixture_drive(6, 0, (unsigned int)f, 30, 0));
	}
	CHECK(!epd_engine_busy(r.e));
	rig_free(&r);
}

TEST(engine_submit_rejects_bad_requests)
{
	struct rig r;

	rig_init(&r);
	CHECK_EQ(submit(&r, 0, 0, W + 1, 1, GC16, true, 1), -EINVAL);
	CHECK_EQ(submit(&r, W - 7, 0, 8, 1, GC16, true, 1), -EINVAL);
	CHECK_EQ(submit(&r, 0, H, 8, 1, GC16, true, 1), -EINVAL);
	CHECK_EQ(submit(&r, 0, 0, 8, 1, 3, true, 1), -EINVAL);
	CHECK_EQ(submit(&r, 0, 0, 8, 1, 17, true, 1), -EINVAL);
	CHECK_EQ(submit(&r, 0, 0, 8, 1, -1, true, 1), -EINVAL);
	CHECK(!epd_engine_busy(r.e));
	CHECK_EQ(r.ndone, 0);
	rig_free(&r);
}

TEST(engine_mode_14_is_built_in)
{
	static const u8 want[30] = { 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1,
				     2, 2, 2, 2, 2, 2, 2, 2, 0 };
	struct rig r;
	int f;

	rig_init(&r);
	fill(&r, 3);
	CHECK_EQ(epd_engine_mode_frames(r.e, 14, true), 30);
	CHECK_EQ(submit(&r, 0, 0, W, H, 14, true, 1), 0);
	for (f = 0; f < 30; f++) {
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
		CHECK_EQ(drive_at(&r, 17, 3), want[f]);
	}
	CHECK(!epd_engine_busy(r.e));
	rig_free(&r);
}

static void regal_scene(struct rig *r, bool aa)
{
	struct epd_engine_cfg c = base_cfg(r);
	u32 x, y;

	c.regal_aa = aa;
	rig_init_cfg(r, &c);
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++)
			r->grey[y * W + x] = x < 24 ? 0 : 0xf0;
	CHECK_EQ(submit(r, 0, 0, W, H, GC16, true, 9000), 0);
	run(r);
	r->ndone = 0;
}

TEST(engine_regal_redrives_white_next_to_dark)
{
	struct rig r;
	u32 x, y;
	int f, n = (int)fixture_frames(4, 0);

	regal_scene(&r, true);
	CHECK_EQ(submit(&r, 0, 0, W, H, REGAL, false, 1), 0);
	for (f = 0; f < n; f++) {
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
		for (y = 0; y < H; y++)
			for (x = 0; x < W; x++)
				CHECK_EQ(drive_at(&r, x, y),
					 x == 24 ? fixture_drive(4, 0, (unsigned int)f, 31, 31) : 0);
	}
	CHECK(!epd_engine_busy(r.e));
	CHECK_EQ(r.ndone, 1);
	rig_free(&r);

	regal_scene(&r, false);
	CHECK_EQ(submit(&r, 0, 0, W, H, REGAL, false, 1), 0);
	CHECK(!epd_engine_busy(r.e));
	CHECK_EQ(r.ndone, 1);
	rig_free(&r);
}

TEST(engine_regal_tables_are_fixed_up)
{
	struct rig r;

	regal_scene(&r, false);
	fill(&r, 15);
	CHECK_EQ(submit(&r, 0, 0, 8, 1, REGAL, false, 1), 0);
	CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
	CHECK_EQ(drive_at(&r, 0, 0), fixture_drive(4, 0, 0, 30, 0));
	run(&r);
	fill(&r, 0);
	CHECK_EQ(submit(&r, 0, 0, 8, 1, REGAL, true, 2), 0);
	CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
	CHECK_EQ(drive_at(&r, 0, 0), fixture_drive(4, 0, 0, 0, 30));
	run(&r);
	CHECK_EQ(submit(&r, 0, 0, 8, 1, REGAL, true, 3), 0);
	CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
	CHECK_EQ(drive_at(&r, 0, 0), 0);
	rig_free(&r);
}

TEST(engine_mode_0_uses_slot_0)
{
	struct rig r;

	rig_init(&r);
	fill(&r, 15);
	CHECK_EQ(epd_engine_mode_frames(r.e, 0, true), (int)fixture_frames(0, 0));
	CHECK_EQ(submit(&r, 0, 0, 8, 1, 0, true, 1), 0);
	CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
	CHECK_EQ(drive_at(&r, 0, 0), fixture_drive(0, 0, 0, 30, 0));
	rig_free(&r);
}

TEST(engine_single_frame_mode)
{
	struct rig r;

	rig_init(&r);
	fill(&r, 15);
	CHECK_EQ(epd_engine_mode_frames(r.e, 16, true), 1);
	CHECK_EQ(submit(&r, 0, 0, 8, 1, 16, true, 1), 0);
	CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
	CHECK_EQ(drive_at(&r, 0, 0), fixture_drive(10, 0, 0, 30, 0));
	CHECK(!epd_engine_busy(r.e));
	CHECK_EQ(r.ndone, 1);
	rig_free(&r);
}

TEST(engine_rejects_modes_the_waveform_lacks)
{
	static const u8 bounds[] = { 0, 50 };
	struct wbf_spec s = fixture_spec;
	struct epd_engine_cfg c = { .timing = &fixture_timing };
	struct epd_update u = { .width = 8, .height = 1, .mode = A2, .full = true };
	u8 grey[W * H] = { 0 };
	struct epd_engine *e;
	struct wbf_blob b;
	int err = 0;

	s.n_modes = 6;
	s.n_temps = 1;
	s.temp_bounds = bounds;
	wbf_build(&s, &b);
	e = epd_engine_create(&c, b.data, b.size, &err);
	if (!e)
		abort();
	CHECK_EQ(epd_engine_submit(e, &u, grey, W), -EINVAL);
	CHECK_EQ(epd_engine_mode_frames(e, A2, true), -EINVAL);
	u.mode = REGAL;
	CHECK_EQ(epd_engine_submit(e, &u, grey, W), 0);
	CHECK_EQ(epd_engine_set_cut(e, 0), 0);
	CHECK_EQ(epd_engine_set_cut(e, 255), 0);
	epd_engine_destroy(e);
	free(b.data);
}

TEST(engine_markers_complete_in_submit_order)
{
	struct rig r;
	int f;

	rig_init(&r);
	settle(&r, 0);
	fill(&r, 15);
	CHECK_EQ(submit(&r, 0, 0, 8, 1, DU, true, 1), 0);
	CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
	CHECK_EQ(submit(&r, 16, 0, 8, 1, GC16, true, 2), 0);
	for (f = 0; f < 2; f++)
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
	CHECK_EQ(r.ndone, 1);
	CHECK_EQ(submit(&r, 32, 0, 8, 1, DU, true, 3), 0);
	for (f = 0; f < 3; f++)
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
	CHECK(!epd_engine_busy(r.e));
	CHECK_EQ(r.ndone, 3);
	CHECK_EQ(r.done[1], 2);
	CHECK_EQ(r.done[2], 3);
	rig_free(&r);
}

TEST(engine_tick_keeps_the_frame_when_a_waiting_update_is_dropped)
{
	struct rig r;
	int f, n = (int)fixture_frames(2, 0);

	rig_init(&r);
	settle(&r, 0);
	fill(&r, 12);
	CHECK_EQ(submit(&r, 0, 0, W, H, GC16, true, 1), 0);
	CHECK_EQ(submit(&r, 0, 0, 8, 1, 15, true, 2), 0);
	CHECK_EQ(epd_engine_set_temp(r.e, 45), 0);
	for (f = 0; f < n; f++) {
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
		CHECK_EQ(drive_at(&r, 40, 3), fixture_drive(2, 0, (unsigned int)f, 24, 0));
	}
	CHECK(!epd_engine_busy(r.e));
	CHECK_EQ(r.ndone, 2);
	CHECK_EQ(r.done[0], 1);
	CHECK_EQ(r.done[1], 2);
	rig_free(&r);
}

TEST(engine_destroy_releases_pending_work)
{
	struct rig r;

	rig_init(&r);
	fill(&r, 12);
	CHECK_EQ(submit(&r, 0, 0, W, H, GC16, true, 1), 0);
	CHECK_EQ(submit(&r, 0, 0, W, H, DU, true, 2), 0);
	CHECK_EQ(submit(&r, 0, 0, W, H, A2, true, 3), 0);
	CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
	CHECK_EQ(epd_engine_set_temp(r.e, 40), 0);
	CHECK_EQ(epd_engine_set_cut(r.e, 1), 0);
	rig_free(&r);
	epd_engine_destroy(NULL);
}
TEST(engine_older_waiter_does_not_overwrite_newer_pixels)
{
	struct rig r;
	int f;

	rig_init(&r);
	settle(&r, 0);
	fill(&r, 9);
	CHECK_EQ(submit(&r, 0, 0, 24, H, DU, true, 1), 0);
	CHECK_EQ(submit(&r, 24, 0, 24, H, GC16, true, 2), 0);
	fill(&r, 12);
	CHECK_EQ(submit(&r, 0, 0, W, H, GC16, false, 3), 0);
	for (f = 0; f < (int)fixture_frames(1, 0); f++)
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
	fill(&r, 4);
	CHECK_EQ(submit(&r, 0, 0, 24, H, DU, true, 4), 0);
	run(&r);
	CHECK_EQ(r.ndone, 4);
	CHECK_EQ(r.done[3], 4);
	CHECK_EQ(submit(&r, 0, 0, 24, H, DU, false, 5), 0);
	CHECK(!epd_engine_busy(r.e));
	rig_free(&r);
}
