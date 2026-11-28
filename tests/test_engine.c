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
#define NIGHT	15

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

static void rig_init_wbf(struct rig *r, const struct epd_engine_cfg *cfg, const struct wbf_blob *b)
{
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

static void rig_init_cfg(struct rig *r, const struct epd_engine_cfg *cfg)
{
	rig_init_wbf(r, cfg, fixture_wbf());
}

static void rig_init(struct rig *r)
{
	struct epd_engine_cfg c = base_cfg(r);

	rig_init_cfg(r, &c);
}

static void rig_init_plain(struct rig *r)
{
	struct epd_engine_cfg c = base_cfg(r);

	c.tables.a2_clean_mode = 2;
	rig_init_wbf(r, &c, fixture_plain_wbf());
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

static void fill(struct rig *r, u8 level);
static int run(struct rig *r);

static int submit_repeat(struct rig *r, u16 left, u16 top, u16 w, u16 h, int mode, u32 marker)
{
	struct epd_update u = { .top = top, .left = left, .width = w, .height = h, .mode = mode,
				.full = true, .repeat = true, .marker = marker };

	return epd_engine_submit(r->e, &u, r->grey, W);
}

static bool shows(struct rig *r, u8 level)
{
	u8 keep[W * H];
	int ndone = r->ndone;
	bool same;

	memcpy(keep, r->grey, sizeof(keep));
	fill(r, level);
	CHECK_EQ(submit(r, 0, 0, W, H, GC16, false, 8000), 0);
	same = !epd_engine_busy(r->e);
	run(r);
	memcpy(r->grey, keep, sizeof(keep));
	r->ndone = ndone;
	return same;
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
	CHECK_EQ(epd_engine_set_temp_mc(r.e, 25000), 0);
	CHECK_EQ(epd_engine_mode_frames(r.e, GC16, true), (int)fixture_frames(2, 1));
	fill(&r, 6);
	CHECK_EQ(submit(&r, 0, 0, 8, 1, GC16, true, 1), 0);
	CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
	CHECK_EQ(drive_at(&r, 0, 0), fixture_drive(2, 1, 0, 12, 0));
	CHECK_EQ(epd_engine_set_temp_mc(r.e, 45000), 0);
	CHECK_EQ(run(&r), (int)fixture_frames(2, 1) - 1);
	CHECK_EQ(epd_engine_mode_frames(r.e, GC16, true), (int)fixture_frames(2, 2));
	rig_free(&r);
}

TEST(engine_temperature_holds_the_table_near_a_bound)
{
	struct rig r;
	int lo, hi;

	rig_init(&r);
	epd_engine_temp_range(r.e, &lo, &hi);
	CHECK_EQ(lo, 0);
	CHECK_EQ(hi, 20);
	CHECK_EQ(epd_engine_set_temp_mc(r.e, 20500), 0);
	CHECK_EQ(epd_engine_mode_frames(r.e, GC16, true), (int)fixture_frames(2, 0));
	CHECK_EQ(epd_engine_set_temp_mc(r.e, 21000), 0);
	CHECK_EQ(epd_engine_mode_frames(r.e, GC16, true), (int)fixture_frames(2, 1));
	epd_engine_temp_range(r.e, &lo, &hi);
	CHECK_EQ(lo, 20);
	CHECK_EQ(hi, 30);
	CHECK_EQ(epd_engine_set_temp_mc(r.e, 19500), 0);
	CHECK_EQ(epd_engine_mode_frames(r.e, GC16, true), (int)fixture_frames(2, 1));
	CHECK_EQ(epd_engine_set_temp_mc(r.e, 18000), 0);
	CHECK_EQ(epd_engine_mode_frames(r.e, GC16, true), (int)fixture_frames(2, 0));
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

TEST(engine_regal_pulses_white_next_to_a_changing_dark_pixel)
{
	struct rig r;
	u32 x, y;
	int f, n = (int)fixture_frames(4, 0);

	regal_scene(&r, true);
	r.grey[2 * W + 23] = 0xf0;
	r.grey[3 * W + 23] = 0xf0;
	CHECK_EQ(submit(&r, 0, 0, W, H, REGAL, false, 1), 0);
	for (f = 0; f < n; f++) {
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
		for (y = 0; y < H; y++)
			for (x = 0; x < W; x++) {
				u8 want = 0;

				if (x == 23 && (y == 2 || y == 3))
					want = fixture_drive(4, 0, (unsigned int)f, 30, 0);
				else if (x == 24 && y >= 1 && y <= 4)
					want = fixture_drive(4, 0, (unsigned int)f, 31, 31);
				CHECK_EQ(drive_at(&r, x, y), want);
			}
	}
	CHECK(!epd_engine_busy(r.e));
	CHECK_EQ(r.ndone, 1);
	rig_free(&r);
}

TEST(engine_regal_leaves_edges_of_unchanged_pixels_alone)
{
	struct rig r;

	regal_scene(&r, true);
	CHECK_EQ(submit(&r, 0, 0, W, H, REGAL, false, 1), 0);
	CHECK(!epd_engine_busy(r.e));
	CHECK_EQ(submit(&r, 0, 0, W, H, REGAL, true, 2), 0);
	CHECK(!epd_engine_busy(r.e));
	CHECK_EQ(r.ndone, 2);
	rig_free(&r);
}

TEST(engine_regal_can_pulse_every_old_edge_like_stock)
{
	struct rig r;
	u32 x, y;
	int f, n = (int)fixture_frames(4, 0);

	regal_scene(&r, true);
	epd_engine_set_aa_all(r.e, true);
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
	epd_engine_set_aa_all(r.e, false);
	CHECK_EQ(submit(&r, 0, 0, W, H, REGAL, false, 2), 0);
	CHECK(!epd_engine_busy(r.e));
	rig_free(&r);

	regal_scene(&r, false);
	epd_engine_set_aa_all(r.e, true);
	CHECK_EQ(submit(&r, 0, 0, W, H, REGAL, false, 1), 0);
	CHECK(!epd_engine_busy(r.e));
	CHECK_EQ(r.ndone, 1);
	rig_free(&r);
}

TEST(engine_regal_edge_waits_with_its_pixel)
{
	struct rig r;
	u32 y;
	int f, ng = (int)fixture_frames(2, 0), n = (int)fixture_frames(4, 0);

	regal_scene(&r, true);
	CHECK_EQ(submit(&r, 24, 0, 8, H, GC16, true, 1), 0);
	for (y = 0; y < H; y++)
		r.grey[y * W + 23] = 0xf0;
	CHECK_EQ(submit(&r, 0, 0, W, H, REGAL, false, 2), 0);
	for (f = 0; f < ng; f++) {
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
		CHECK_EQ(drive_at(&r, 24, 2), fixture_drive(2, 0, (unsigned int)f, 30, 30));
		CHECK_EQ(drive_at(&r, 23, 2), f < n ? fixture_drive(4, 0, (unsigned int)f, 30, 0) : 0);
	}
	for (f = 0; f < n; f++) {
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
		CHECK_EQ(drive_at(&r, 24, 2), fixture_drive(4, 0, (unsigned int)f, 31, 31));
		CHECK_EQ(drive_at(&r, 25, 2), 0);
		CHECK_EQ(drive_at(&r, 23, 2), 0);
	}
	CHECK(!epd_engine_busy(r.e));
	CHECK_EQ(r.ndone, 2);
	rig_free(&r);
}

TEST(engine_night_regal_has_no_edge_pulse)
{
	struct rig r;

	regal_scene(&r, true);
	CHECK_EQ(submit(&r, 0, 0, W, H, NIGHT, false, 1), 0);
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
	CHECK(!epd_engine_busy(r.e));
	CHECK_EQ(r.ndone, 3);
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
	CHECK_EQ(epd_engine_set_temp_mc(r.e, 45000), 0);
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
	CHECK_EQ(epd_engine_set_temp_mc(r.e, 40000), 0);
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

TEST(engine_update_without_drive_takes_no_lut)
{
	struct rig r;

	rig_init_plain(&r);
	settle(&r, 15);
	fill(&r, 15);
	CHECK_EQ(submit(&r, 0, 0, W, H, DU, true, 1), 0);
	CHECK(!epd_engine_busy(r.e));
	CHECK_EQ(r.ndone, 1);
	CHECK_EQ(submit(&r, 0, 0, W, H, A2, true, 2), 0);
	CHECK(!epd_engine_busy(r.e));
	CHECK_EQ(r.ndone, 2);
	rig_free(&r);
}

TEST(engine_two_level_waveform_gets_two_level_targets)
{
	struct rig r;

	rig_init_plain(&r);
	settle(&r, 15);
	fill(&r, 7);
	CHECK_EQ(submit(&r, 0, 0, W, H, DU, false, 1), 0);
	CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
	CHECK_EQ(drive_at(&r, 5, 2), 1);
	run(&r);
	CHECK(shows(&r, 0));
	fill(&r, 8);
	CHECK_EQ(submit(&r, 0, 0, W, H, DU, false, 2), 0);
	CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
	CHECK_EQ(drive_at(&r, 5, 2), 2);
	run(&r);
	CHECK(shows(&r, 15));
	rig_free(&r);
}

TEST(engine_a2_uses_du_from_greys)
{
	struct rig r;
	u8 level;

	for (level = 1; level < 15; level++) {
		rig_init_plain(&r);
		settle(&r, level);
		fill(&r, level < 8 ? 15 : 0);
		CHECK_EQ(submit(&r, 0, 0, W, H, A2, true, 1), 0);
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
		CHECK_EQ(drive_at(&r, 5, 2), level < 8 ? 2 : 1);
		CHECK_EQ(run(&r), (int)fixture_plain_frames(1, 0) - 1);
		CHECK(shows(&r, level < 8 ? 15 : 0));
		rig_free(&r);
	}
}

TEST(engine_unreachable_level_leaves_the_pixel)
{
	struct rig r;

	rig_init_plain(&r);
	settle(&r, 0);
	fill(&r, 7);
	CHECK_EQ(submit(&r, 0, 0, W, H, NIGHT, false, 1), 0);
	CHECK(!epd_engine_busy(r.e));
	CHECK_EQ(r.ndone, 1);
	CHECK(shows(&r, 0));
	rig_free(&r);
}

static void a2_passes(u8 from, const u8 *steps, size_t n)
{
	struct rig r;
	size_t i;

	rig_init_plain(&r);
	settle(&r, from);
	for (i = 0; i + 1 < n; i += 2) {
		fill(&r, steps[i]);
		CHECK_EQ(submit(&r, 0, 0, W, H, A2, true, 1), 0);
		CHECK_EQ(run(&r), (int)fixture_plain_frames(6, 0));
		CHECK_EQ(epd_engine_level(r.e, 5, 2), steps[i + 1]);
		CHECK_EQ(epd_engine_level(r.e, W - 1, H - 1), steps[i + 1]);
	}
	CHECK_EQ(epd_engine_level(r.e, W, 0), -EINVAL);
	CHECK_EQ(epd_engine_level(r.e, 0, H), -EINVAL);
	rig_free(&r);
}

TEST(engine_a2_pass_ends_at_stock_stage_level)
{
	static const u8 white[] = { 15, 12, 15, 13, 15, 14, 15, 15 };
	static const u8 black[] = { 0, 3, 0, 2, 0, 1, 0, 0 };
	static const u8 back1[] = { 0, 3, 15, 14, 0, 3 };
	static const u8 back2[] = { 0, 3, 0, 2, 15, 13, 0, 2 };
	static const u8 back3[] = { 0, 3, 0, 2, 0, 1, 15, 12, 0, 1 };

	a2_passes(0, white, ARRAY_LEN(white));
	a2_passes(15, black, ARRAY_LEN(black));
	a2_passes(15, back1, ARRAY_LEN(back1));
	a2_passes(15, back2, ARRAY_LEN(back2));
	a2_passes(15, back3, ARRAY_LEN(back3));
}

TEST(engine_stage_level_becomes_a_grey_once_an_update_accepts_it)
{
	struct rig r;

	rig_init_plain(&r);
	settle(&r, 0);
	fill(&r, 15);
	CHECK_EQ(submit(&r, 0, 0, W, H, A2, true, 1), 0);
	CHECK_EQ(run(&r), (int)fixture_plain_frames(6, 0));
	CHECK(shows(&r, 12));
	fill(&r, 15);
	CHECK_EQ(submit(&r, 0, 0, W, H, A2, true, 3), 0);
	CHECK_EQ(run(&r), (int)fixture_plain_frames(1, 0));
	CHECK_EQ(epd_engine_level(r.e, 5, 2), 15);
	rig_free(&r);
}

TEST(engine_a2_repeat_runs_until_the_target)
{
	struct rig r;
	int n = (int)fixture_plain_frames(6, 0), f;

	rig_init_plain(&r);
	settle(&r, 0);
	fill(&r, 15);
	CHECK_EQ(submit_repeat(&r, 0, 0, W, H, A2, 5), 0);
	for (f = 0; f < 4 * n; f++) {
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
		CHECK_EQ(drive_at(&r, 5, 2), f % n == n - 1 ? 0 : 2);
		CHECK_EQ(r.ndone, f >= n - 1);
	}
	CHECK(!epd_engine_busy(r.e));
	CHECK(shows(&r, 15));

	fill(&r, 0);
	CHECK_EQ(submit_repeat(&r, 0, 0, W, H, A2, 6), 0);
	CHECK_EQ(run(&r), 4 * n);
	CHECK_EQ(r.ndone, 2);
	CHECK(shows(&r, 0));
	rig_free(&r);
}

TEST(engine_a2_repeat_covers_only_its_pixels)
{
	struct rig r;
	u32 x;

	rig_init_plain(&r);
	settle(&r, 0);
	fill(&r, 12);
	CHECK_EQ(submit(&r, 0, 0, W, H, GC16, true, 1), 0);
	run(&r);
	for (x = 0; x < 8; x++)
		r.grey[x] = 0;
	CHECK_EQ(submit(&r, 0, 0, W, H, GC16, false, 2), 0);
	run(&r);
	for (x = 0; x < 8; x++)
		r.grey[x] = 0xff;
	CHECK_EQ(submit_repeat(&r, 0, 0, 8, 1, A2, 3), 0);
	run(&r);
	for (x = 0; x < W; x++)
		r.grey[x] = x < 8 ? 0xf0 : 0xc0;
	CHECK_EQ(submit(&r, 0, 0, W, H, GC16, false, 4), 0);
	CHECK(!epd_engine_busy(r.e));
	rig_free(&r);
}

TEST(engine_newer_update_ends_a_repeat)
{
	struct rig r;
	int n = (int)fixture_plain_frames(6, 0), f;

	rig_init_plain(&r);
	settle(&r, 0);
	fill(&r, 15);
	CHECK_EQ(submit_repeat(&r, 0, 0, W, H, A2, 1), 0);
	for (f = 0; f < n; f++)
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
	fill(&r, 0);
	CHECK_EQ(submit(&r, 0, 0, W, H, A2, true, 2), 0);
	for (f = 0; f < n; f++) {
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
		CHECK_EQ(drive_at(&r, 5, 2), f == n - 1 ? 0 : 2);
	}
	for (f = 0; f < n; f++) {
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
		CHECK_EQ(drive_at(&r, 5, 2), f == n - 1 ? 0 : 1);
	}
	CHECK(!epd_engine_busy(r.e));
	CHECK_EQ(r.ndone, 2);
	CHECK(shows(&r, 2));
	rig_free(&r);
}

TEST(engine_unchanged_pixel_of_a_newer_update_ends_a_repeat)
{
	struct rig r;
	int n = (int)fixture_plain_frames(6, 0), f;

	rig_init_plain(&r);
	settle(&r, 0);
	fill(&r, 15);
	CHECK_EQ(submit_repeat(&r, 0, 0, W, H, A2, 1), 0);
	for (f = 0; f < n; f++)
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
	fill(&r, 13);
	CHECK_EQ(submit(&r, 0, 0, W, H, GC16, false, 2), 0);
	for (f = 0; f < n; f++)
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
	CHECK(!epd_engine_busy(r.e));
	CHECK_EQ(r.ndone, 2);
	CHECK(shows(&r, 13));
	rig_free(&r);
}

TEST(engine_repeat_only_applies_to_staged_waveforms)
{
	struct rig r;

	rig_init_plain(&r);
	settle(&r, 0);
	fill(&r, 15);
	CHECK_EQ(submit_repeat(&r, 0, 0, W, H, DU, 1), 0);
	CHECK_EQ(run(&r), (int)fixture_plain_frames(1, 0));
	CHECK(shows(&r, 15));
	rig_free(&r);
}

TEST(engine_last_a2_repeat_touches_up_the_neighbours)
{
	struct rig r;
	int n = (int)fixture_plain_frames(6, 0), f;
	u32 x, y;

	rig_init_plain(&r);
	settle(&r, 15);
	r.grey[2 * W + 10] = 0;
	CHECK_EQ(submit_repeat(&r, 10, 2, 1, 1, A2, 1), 0);
	for (f = 0; f < 4 * n; f++) {
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
		CHECK_EQ(drive_at(&r, 10, 2), f % n == n - 1 ? 0 : 1);
		CHECK_EQ(drive_at(&r, 9, 2), 0);
		CHECK_EQ(drive_at(&r, 11, 3), 0);
	}
	CHECK(!epd_engine_busy(r.e));
	r.grey[2 * W + 10] = 0xff;
	CHECK_EQ(submit_repeat(&r, 10, 2, 1, 1, A2, 2), 0);
	for (f = 0; f < 4 * n; f++) {
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
		for (y = 0; y < H; y++)
			for (x = 0; x < W; x++) {
				bool self = x == 10 && y == 2;
				bool near = x >= 9 && x <= 11 && y >= 1 && y <= 3 && !self;
				u8 want = f % n == n - 1 ? 0 : self || (near && f >= 3 * n) ? 2 : 0;

				CHECK_EQ(drive_at(&r, x, y), want);
			}
	}
	CHECK(!epd_engine_busy(r.e));
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++)
			CHECK_EQ(epd_engine_level(r.e, x, y), 15);
	rig_free(&r);
}

TEST(engine_a2_touch_up_reaches_past_the_update_and_stops_at_the_screen_edge)
{
	struct rig r;
	int n = (int)fixture_plain_frames(6, 0), f;

	rig_init_plain(&r);
	settle(&r, 0);
	r.grey[0] = 0xff;
	r.grey[(H - 1) * W + W - 1] = 0xff;
	CHECK_EQ(submit_repeat(&r, 0, 0, 1, 1, A2, 1), 0);
	CHECK_EQ(submit_repeat(&r, W - 1, H - 1, 1, 1, A2, 2), 0);
	run(&r);
	r.grey[0] = 0;
	r.grey[(H - 1) * W + W - 1] = 0;
	CHECK_EQ(submit_repeat(&r, 0, 0, 1, 1, A2, 3), 0);
	CHECK_EQ(submit_repeat(&r, W - 1, H - 1, 1, 1, A2, 4), 0);
	for (f = 0; f < 4 * n; f++) {
		CHECK_EQ(epd_engine_tick(r.e, r.frame), 1);
		CHECK_EQ(drive_at(&r, 1, 1), f >= 3 * n && f % n != n - 1 ? 1 : 0);
		CHECK_EQ(drive_at(&r, W - 2, H - 2), f >= 3 * n && f % n != n - 1 ? 1 : 0);
		CHECK_EQ(drive_at(&r, 2, 0), 0);
	}
	CHECK(!epd_engine_busy(r.e));
	CHECK_EQ(r.ndone, 4);
	rig_free(&r);
}
