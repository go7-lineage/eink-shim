// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>
#include <string.h>

#include "epd_ui.h"
#include "test.h"

#define W	32
#define H	16
#define PITCH	(W * 4 + 16)
#define LEN	(PITCH * H)

struct ui_rig {
	struct epd_ui ui;
	struct epd_ui_cfg cfg;
	u8 fb[LEN];
	struct epd_request req[EPD_UI_MAX_RECTS];
	int n;
	int cut;
};

static void ui_rig_init(struct ui_rig *r, int mode)
{
	unsigned int seed = 5;
	size_t i;

	memset(r, 0, sizeof(*r));
	for (i = 0; i < LEN; i++)
		r->fb[i] = (u8)test_rand(&seed);
	CHECK_EQ(epd_ui_init(&r->ui, W, H), 0);
	r->cfg = (struct epd_ui_cfg){ .mode = mode, .gc_interval = 0, .big_pct = 0, .a2cut = 3 };
}

static void poke(struct ui_rig *r, u32 x, u32 y)
{
	r->fb[y * PITCH + 4 * x + 1] ^= 0x40;
}

static void poke_rows(struct ui_rig *r, u32 y0, u32 y1)
{
	u32 x, y;

	for (y = y0; y < y1; y++)
		for (x = 0; x < W; x++)
			poke(r, x, y);
}

static void poke_span(struct ui_rig *r, u32 x0, u32 x1, u32 y)
{
	u32 x;

	for (x = x0; x < x1; x++)
		poke(r, x, y);
}

static int frame(struct ui_rig *r, u64 now)
{
	memset(r->req, 0xee, sizeof(r->req));
	r->cut = 1234;
	r->n = epd_ui_frame(&r->ui, &r->cfg, r->fb, PITCH, LEN, now, r->req, &r->cut);
	return r->n;
}

static enum epd_ui_step step(struct ui_rig *r, u64 now, u64 *wait)
{
	memset(r->req, 0xee, sizeof(r->req));
	r->n = -1;
	return epd_ui_step(&r->ui, now, r->req, &r->n, wait);
}

#define EXPECT_RECT(r, i, x, y, w, h, wf, fl, um)	\
	do {						\
		CHECK_EQ((r)->req[i].left, x);		\
		CHECK_EQ((r)->req[i].top, y);		\
		CHECK_EQ((r)->req[i].width, w);		\
		CHECK_EQ((r)->req[i].height, h);	\
		CHECK_EQ((r)->req[i].mode, wf);		\
		CHECK_EQ((r)->req[i].flags, fl);	\
		CHECK_EQ((r)->req[i].update_mode, um);	\
	} while (0)

#define EXPECT_REQ(r, x, y, w, h, wf, fl, um)		\
	do {						\
		CHECK_EQ((r)->n, 1);			\
		EXPECT_RECT(r, 0, x, y, w, h, wf, fl, um);	\
	} while (0)

TEST(ui_init_validates_size)
{
	struct epd_ui u;

	CHECK_EQ(epd_ui_init(&u, 0, 8), -EINVAL);
	CHECK_EQ(epd_ui_init(&u, 31, 8), -EINVAL);
	CHECK_EQ(epd_ui_init(&u, 32, 0), -EINVAL);
	CHECK_EQ(epd_ui_init(&u, 0x10000, 8), -EINVAL);
	CHECK_EQ(epd_ui_init(&u, 32, 8), 0);
	epd_ui_release(&u);
	epd_ui_release(&u);
	CHECK_EQ(epd_ui_init(&u, 2, 1), 0);
	epd_ui_release(&u);
}

TEST(ui_mode_names)
{
	CHECK_EQ(epd_ui_mode_by_name("speed"), EPD_UI_SPEED);
	CHECK_EQ(epd_ui_mode_by_name("normal"), EPD_UI_NORMAL);
	CHECK_EQ(epd_ui_mode_by_name("regal"), EPD_UI_REGAL);
	CHECK_EQ(epd_ui_mode_by_name("regalplus"), EPD_UI_REGALPLUS);
	CHECK_EQ(epd_ui_mode_by_name("night"), EPD_UI_NIGHT);
	CHECK_EQ(epd_ui_mode_by_name("fast"), -EINVAL);
	CHECK_EQ(epd_ui_mode_by_name(""), -EINVAL);
	CHECK_EQ(epd_ui_mode_by_name("speed "), -EINVAL);
	CHECK(!strcmp(epd_ui_mode_name(EPD_UI_SPEED), "speed"));
	CHECK(!strcmp(epd_ui_mode_name(EPD_UI_REGALPLUS), "regalplus"));
	CHECK(!strcmp(epd_ui_mode_name(EPD_UI_NIGHT), "night"));
	CHECK(!strcmp(epd_ui_mode_name(EPD_UI_NMODES), "?"));
	CHECK(!strcmp(epd_ui_mode_name(-1), "?"));
}

TEST(ui_first_frame_is_the_whole_screen)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_SPEED);
	CHECK_EQ(frame(&r, 1000), 1);
	EXPECT_REQ(&r, 0, 0, W, H, 4, 0x2000, 0);
	CHECK_EQ(r.req[0].marker, 1);
	CHECK_EQ(r.cut, 3);
	CHECK_EQ(step(&r, 1000, &wait), EPD_UI_IDLE);
	epd_ui_release(&r.ui);
}

TEST(ui_unchanged_frame_sends_nothing)
{
	struct ui_rig r;

	ui_rig_init(&r, EPD_UI_SPEED);
	CHECK_EQ(frame(&r, 0), 1);
	CHECK_EQ(frame(&r, 1000), 0);
	CHECK_EQ(r.cut, -1);
	r.fb[3 * PITCH + W * 4 + 5] ^= 0xff;
	CHECK_EQ(frame(&r, 2000), 0);
	epd_ui_release(&r.ui);
}

TEST(ui_changed_area_is_aligned_to_bytes_of_pixels)
{
	struct ui_rig r;

	ui_rig_init(&r, EPD_UI_SPEED);
	CHECK_EQ(frame(&r, 0), 1);
	poke(&r, 13, 5);
	CHECK_EQ(frame(&r, 1000), 1);
	EXPECT_REQ(&r, 8, 5, 8, 1, 4, 0x2000, 0);
	CHECK_EQ(r.req[0].marker, 2);
	CHECK_EQ(r.cut, -1);
	poke(&r, 3, 2);
	poke(&r, 29, 6);
	CHECK_EQ(frame(&r, 2000), 2);
	EXPECT_RECT(&r, 0, 0, 2, 8, 1, 4, 0x2000, 0);
	EXPECT_RECT(&r, 1, 24, 6, 8, 1, 4, 0x2000, 0);
	poke(&r, 31, 7);
	CHECK_EQ(frame(&r, 3000), 1);
	EXPECT_REQ(&r, 24, 7, 8, 1, 4, 0x2000, 0);
	CHECK_EQ(frame(&r, 4000), 0);
	epd_ui_release(&r.ui);
}

TEST(ui_accepts_a_pitch_without_padding)
{
	static u8 fb[W * 4 * H];
	struct epd_ui_cfg cfg = { .mode = EPD_UI_SPEED };
	struct epd_request req[EPD_UI_MAX_RECTS];
	struct epd_ui u;
	int cut;

	CHECK_EQ(epd_ui_init(&u, W, H), 0);
	CHECK_EQ(epd_ui_frame(&u, &cfg, fb, W * 4, sizeof(fb), 0, req, &cut), 1);
	CHECK_EQ(epd_ui_frame(&u, &cfg, fb, W * 4, sizeof(fb), 1000, req, &cut), 0);
	epd_ui_release(&u);
}

TEST(ui_modes_pick_waveform_and_flags)
{
	static const struct { int mode, wf; u32 flag; } want[] = {
		{ EPD_UI_SPEED, 4, 0x2000 }, { EPD_UI_NORMAL, 255, 0 },
		{ EPD_UI_REGAL, 5, 0x1000 }, { EPD_UI_REGALPLUS, 16, 0 }, { EPD_UI_NIGHT, 15, 0x1000 },
		{ 99, 4, 0x2000 },
	};
	struct ui_rig r;
	size_t i;

	for (i = 0; i < ARRAY_LEN(want); i++) {
		ui_rig_init(&r, want[i].mode);
		CHECK_EQ(frame(&r, 0), 1);
		EXPECT_REQ(&r, 0, 0, W, H, want[i].wf, want[i].flag, 0);
		epd_ui_release(&r.ui);
	}
}

TEST(ui_mode_change_refreshes_and_reports_the_cut)
{
	struct ui_rig r;

	ui_rig_init(&r, EPD_UI_SPEED);
	r.cfg.a2cut = 20;
	CHECK_EQ(frame(&r, 0), 1);
	CHECK_EQ(r.cut, 9);
	r.cfg.mode = EPD_UI_NORMAL;
	poke(&r, 0, 0);
	CHECK_EQ(frame(&r, 1000), 1);
	EXPECT_REQ(&r, 0, 0, W, H, 2, 0x1000, 1);
	CHECK_EQ(r.cut, 0);
	r.cfg.mode = EPD_UI_SPEED;
	r.cfg.a2cut = -5;
	poke(&r, 0, 0);
	CHECK_EQ(frame(&r, 2000), 1);
	EXPECT_REQ(&r, 0, 0, W, H, 2, 0x1000, 1);
	CHECK_EQ(r.cut, -1);
	poke(&r, 0, 0);
	CHECK_EQ(frame(&r, 3000), 1);
	EXPECT_REQ(&r, 0, 0, 8, 1, 4, 0x2000, 0);
	CHECK_EQ(r.cut, -1);
	epd_ui_release(&r.ui);
}

TEST(ui_gc_interval_counts_frames_after_quiet)
{
	struct ui_rig r;
	u64 t = 0;
	int i;

	ui_rig_init(&r, EPD_UI_SPEED);
	r.cfg.gc_interval = 2;
	CHECK_EQ(frame(&r, t), 1);
	for (i = 0; i < 5; i++) {
		t += 499;
		poke(&r, 0, 0);
		CHECK_EQ(frame(&r, t), 1);
		EXPECT_REQ(&r, 0, 0, 8, 1, 4, 0x2000, 0);
	}
	t += 500;
	poke(&r, 0, 0);
	CHECK_EQ(frame(&r, t), 1);
	EXPECT_REQ(&r, 0, 0, 8, 1, 4, 0x2000, 0);
	t += 500;
	poke(&r, 0, 0);
	CHECK_EQ(frame(&r, t), 1);
	EXPECT_REQ(&r, 0, 0, W, H, 2, 0x1000, 1);
	t += 500;
	poke(&r, 0, 0);
	CHECK_EQ(frame(&r, t), 1);
	EXPECT_REQ(&r, 0, 0, 8, 1, 4, 0x2000, 0);
	epd_ui_release(&r.ui);
}

TEST(ui_gc_interval_gc16_covers_only_rows_changed_since_the_last_gc16)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_SPEED);
	r.cfg.gc_interval = 2;
	CHECK_EQ(frame(&r, 0), 1);
	CHECK_EQ(epd_ui_seed(&r.ui, r.fb, PITCH, LEN, 1), 0);
	poke(&r, 9, 1);
	CHECK_EQ(frame(&r, 1000), 1);
	poke(&r, 17, 5);
	CHECK_EQ(frame(&r, 2000), 1);
	poke(&r, 1, 3);
	CHECK_EQ(frame(&r, 3000), 3);
	EXPECT_RECT(&r, 0, 8, 1, 8, 1, 2, 0x1000, 1);
	EXPECT_RECT(&r, 1, 0, 3, 8, 1, 2, 0x1000, 1);
	EXPECT_RECT(&r, 2, 16, 5, 8, 1, 2, 0x1000, 1);
	CHECK_EQ(step(&r, 5000, &wait), EPD_UI_IDLE);

	poke(&r, 1, 3);
	CHECK_EQ(frame(&r, 6000), 1);
	EXPECT_REQ(&r, 0, 3, 8, 1, 4, 0x2000, 0);
	poke(&r, 25, 7);
	CHECK_EQ(frame(&r, 7000), 1);
	poke(&r, 1, 3);
	CHECK_EQ(frame(&r, 8000), 2);
	EXPECT_RECT(&r, 0, 0, 3, 8, 1, 2, 0x1000, 1);
	EXPECT_RECT(&r, 1, 24, 7, 8, 1, 2, 0x1000, 1);
	epd_ui_release(&r.ui);
}

TEST(ui_mode_change_gc16_stays_full_screen_with_few_dirty_rows)
{
	struct ui_rig r;

	ui_rig_init(&r, EPD_UI_SPEED);
	CHECK_EQ(frame(&r, 0), 1);
	CHECK_EQ(epd_ui_seed(&r.ui, r.fb, PITCH, LEN, 1), 0);
	poke(&r, 9, 1);
	CHECK_EQ(frame(&r, 1000), 1);
	r.cfg.mode = EPD_UI_REGAL;
	poke(&r, 9, 1);
	CHECK_EQ(frame(&r, 2000), 1);
	EXPECT_REQ(&r, 0, 0, W, H, 2, 0x1000, 1);
	epd_ui_release(&r.ui);
}

TEST(ui_gc_interval_gc16_is_full_screen_after_an_unreadable_frame)
{
	struct ui_rig r;

	ui_rig_init(&r, EPD_UI_SPEED);
	r.cfg.gc_interval = 2;
	CHECK_EQ(frame(&r, 0), 1);
	CHECK_EQ(epd_ui_seed(&r.ui, r.fb, PITCH, LEN, 1), 0);
	r.n = epd_ui_frame(&r.ui, &r.cfg, r.fb, W * 4 - 1, LEN, 1000, r.req, &r.cut);
	EXPECT_REQ(&r, 0, 0, W, H, 4, 0x2000, 0);
	poke(&r, 9, 1);
	CHECK_EQ(frame(&r, 2000), 1);
	poke(&r, 9, 1);
	CHECK_EQ(frame(&r, 3000), 1);
	EXPECT_REQ(&r, 0, 0, W, H, 2, 0x1000, 1);

	poke(&r, 9, 1);
	CHECK_EQ(frame(&r, 4000), 1);
	poke(&r, 9, 1);
	CHECK_EQ(frame(&r, 5000), 1);
	poke(&r, 9, 1);
	CHECK_EQ(frame(&r, 6000), 1);
	EXPECT_REQ(&r, 8, 1, 8, 1, 2, 0x1000, 1);
	epd_ui_release(&r.ui);
}

TEST(ui_gc_interval_gc16_serves_a_pending_flash_full_screen)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_SPEED);
	r.cfg.gc_interval = 1;
	CHECK_EQ(frame(&r, 0), 1);
	CHECK_EQ(epd_ui_seed(&r.ui, r.fb, PITCH, LEN, 1), 0);
	poke(&r, 9, 1);
	CHECK_EQ(frame(&r, 1000), 1);
	CHECK_EQ(epd_ui_flash(&r.ui, 1900), 0);
	poke(&r, 9, 1);
	CHECK_EQ(frame(&r, 2000), 1);
	EXPECT_REQ(&r, 0, 0, W, H, 2, 0x1000, 1);
	CHECK_EQ(r.ui.flashes, 1);
	CHECK_EQ(step(&r, 5000, &wait), EPD_UI_IDLE);
	epd_ui_release(&r.ui);
}

TEST(ui_big_burst_pass_takes_its_rows_out_of_the_gc_interval_gc16)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_SPEED);
	r.cfg.gc_interval = 3;
	r.cfg.big_pct = 50;
	CHECK_EQ(frame(&r, 0), 1);
	CHECK_EQ(step(&r, 400, &wait), EPD_UI_SEND);
	EXPECT_REQ(&r, 0, 0, W, H, 2, 0x1000, 1);
	poke_rows(&r, 0, 10);
	CHECK_EQ(frame(&r, 1000), 1);
	CHECK_EQ(step(&r, 1400, &wait), EPD_UI_SEND);
	EXPECT_REQ(&r, 0, 0, W, 10, 2, 0x1000, 1);
	poke(&r, 9, 12);
	CHECK_EQ(frame(&r, 2000), 1);
	poke(&r, 9, 12);
	CHECK_EQ(frame(&r, 3000), 1);
	EXPECT_REQ(&r, 8, 12, 8, 1, 2, 0x1000, 1);
	epd_ui_release(&r.ui);
}

TEST(ui_big_burst_pass_keeps_dirty_rows_reaching_outside_it)
{
	struct ui_rig r;
	u64 wait = 0;
	u32 y;

	ui_rig_init(&r, EPD_UI_SPEED);
	r.cfg.gc_interval = 3;
	r.cfg.big_pct = 50;
	CHECK_EQ(frame(&r, 0), 1);
	CHECK_EQ(epd_ui_seed(&r.ui, r.fb, PITCH, LEN, 1), 0);
	poke(&r, 9, 2);
	CHECK_EQ(frame(&r, 1000), 1);
	for (y = 0; y < H; y++)
		poke_span(&r, 16, W, y);
	CHECK_EQ(frame(&r, 2000), 1);
	CHECK_EQ(step(&r, 2400, &wait), EPD_UI_SEND);
	EXPECT_REQ(&r, 16, 0, 16, H, 2, 0x1000, 1);
	poke(&r, 9, 2);
	CHECK_EQ(frame(&r, 3000), 1);
	poke(&r, 9, 2);
	CHECK_EQ(frame(&r, 4000), 1);
	EXPECT_REQ(&r, 8, 2, 24, 1, 2, 0x1000, 1);
	epd_ui_release(&r.ui);
}

TEST(ui_full_screen_big_burst_pass_cleans_after_an_unreadable_frame)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_SPEED);
	r.cfg.gc_interval = 3;
	r.cfg.big_pct = 50;
	CHECK_EQ(frame(&r, 0), 1);
	CHECK_EQ(epd_ui_seed(&r.ui, r.fb, PITCH, LEN, 1), 0);
	r.n = epd_ui_frame(&r.ui, &r.cfg, r.fb, W * 4 - 1, LEN, 1000, r.req, &r.cut);
	EXPECT_REQ(&r, 0, 0, W, H, 4, 0x2000, 0);
	poke_rows(&r, 0, 10);
	CHECK_EQ(frame(&r, 1050), 1);
	CHECK_EQ(step(&r, 1450, &wait), EPD_UI_SEND);
	EXPECT_REQ(&r, 0, 0, W, H, 2, 0x1000, 1);
	poke(&r, 9, 12);
	CHECK_EQ(frame(&r, 2000), 1);
	poke(&r, 9, 12);
	CHECK_EQ(frame(&r, 3000), 1);
	poke(&r, 9, 12);
	CHECK_EQ(frame(&r, 4000), 1);
	EXPECT_REQ(&r, 8, 12, 8, 1, 2, 0x1000, 1);
	epd_ui_release(&r.ui);
}

TEST(ui_debounced_mode_settles_after_a_burst)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_NORMAL);
	CHECK_EQ(frame(&r, 0), 1);
	EXPECT_REQ(&r, 0, 0, W, H, 255, 0, 0);
	CHECK_EQ(step(&r, 10, &wait), EPD_UI_IDLE);
	poke(&r, 9, 1);
	CHECK_EQ(frame(&r, 99), 1);
	EXPECT_REQ(&r, 8, 1, 8, 1, 4, 0x2000, 0);
	CHECK_EQ(step(&r, 149, &wait), EPD_UI_WAIT);
	CHECK_EQ(wait, 100);
	CHECK_EQ(step(&r, 248, &wait), EPD_UI_WAIT);
	CHECK_EQ(wait, 1);
	CHECK_EQ(step(&r, 249, &wait), EPD_UI_SEND);
	EXPECT_REQ(&r, 8, 1, 8, 1, 255, 0, 1);
	CHECK_EQ(r.req[0].marker, 3);
	CHECK_EQ(step(&r, 250, &wait), EPD_UI_IDLE);
	poke(&r, 9, 1);
	CHECK_EQ(frame(&r, 349), 1);
	EXPECT_REQ(&r, 8, 1, 8, 1, 255, 0, 0);
	CHECK_EQ(step(&r, 350, &wait), EPD_UI_IDLE);
	epd_ui_release(&r.ui);
}

TEST(ui_big_burst_gets_gc16_after_quiet)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_SPEED);
	r.cfg.big_pct = 50;
	CHECK_EQ(frame(&r, 0), 1);
	CHECK_EQ(step(&r, 399, &wait), EPD_UI_WAIT);
	CHECK_EQ(wait, 1);
	CHECK_EQ(step(&r, 400, &wait), EPD_UI_SEND);
	EXPECT_REQ(&r, 0, 0, W, H, 2, 0x1000, 1);
	CHECK_EQ(step(&r, 401, &wait), EPD_UI_IDLE);

	poke_rows(&r, 0, 6);
	CHECK_EQ(frame(&r, 1000), 1);
	CHECK_EQ(step(&r, 2000, &wait), EPD_UI_IDLE);
	poke_rows(&r, 6, 12);
	CHECK_EQ(frame(&r, 1399), 1);
	CHECK_EQ(step(&r, 1500, &wait), EPD_UI_WAIT);
	CHECK_EQ(step(&r, 1799, &wait), EPD_UI_SEND);
	EXPECT_REQ(&r, 0, 0, W, 12, 2, 0x1000, 1);

	poke_rows(&r, 0, 6);
	CHECK_EQ(frame(&r, 3000), 1);
	poke_rows(&r, 6, 12);
	CHECK_EQ(frame(&r, 3400), 1);
	CHECK_EQ(step(&r, 5000, &wait), EPD_UI_IDLE);
	epd_ui_release(&r.ui);
}

TEST(ui_big_burst_threshold_is_exact)
{
	struct ui_rig r;
	u64 wait = 0;
	u32 x;

	ui_rig_init(&r, EPD_UI_SPEED);
	CHECK_EQ(frame(&r, 0), 1);
	r.cfg.big_pct = 50;
	poke_rows(&r, 0, 7);
	for (x = 0; x < 30; x++)
		poke(&r, x, 7);
	CHECK_EQ(frame(&r, 1000), 1);
	CHECK_EQ(step(&r, 5000, &wait), EPD_UI_IDLE);
	poke_rows(&r, 0, 8);
	CHECK_EQ(frame(&r, 6000), 1);
	CHECK_EQ(step(&r, 9000, &wait), EPD_UI_SEND);
	epd_ui_release(&r.ui);
}

TEST(ui_tenth_frame_of_a_burst_still_counts)
{
	struct ui_rig r;
	u64 wait = 0, t = 1000;
	int i;

	ui_rig_init(&r, EPD_UI_SPEED);
	CHECK_EQ(frame(&r, 0), 1);
	r.cfg.big_pct = 50;
	for (i = 0; i < 9; i++, t += 50) {
		poke(&r, 0, 0);
		CHECK_EQ(frame(&r, t), 1);
	}
	poke_rows(&r, 0, H);
	CHECK_EQ(frame(&r, t), 1);
	CHECK_EQ(step(&r, t + 5000, &wait), EPD_UI_SEND);
	epd_ui_release(&r.ui);
}

TEST(ui_gc16_cancels_a_pending_big_burst)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_SPEED);
	r.cfg.big_pct = 50;
	CHECK_EQ(frame(&r, 0), 1);
	r.cfg.mode = EPD_UI_REGAL;
	poke(&r, 0, 0);
	CHECK_EQ(frame(&r, 100), 1);
	EXPECT_REQ(&r, 0, 0, W, H, 2, 0x1000, 1);
	CHECK_EQ(step(&r, 5000, &wait), EPD_UI_IDLE);
	epd_ui_release(&r.ui);
}

TEST(ui_regal_modes_are_debounced)
{
	static const struct { int mode, wf; u32 flag; } m[] = {
		{ EPD_UI_REGAL, 5, 0x1000 }, { EPD_UI_REGALPLUS, 16, 0 }, { EPD_UI_NIGHT, 15, 0x1000 },
		{ EPD_UI_SPEED, 4, 0x2000 },
	};
	struct ui_rig r;
	u64 wait = 0;
	size_t i;

	for (i = 0; i < ARRAY_LEN(m); i++) {
		ui_rig_init(&r, m[i].mode);
		CHECK_EQ(frame(&r, 0), 1);
		poke(&r, 0, 0);
		CHECK_EQ(frame(&r, 50), 1);
		EXPECT_REQ(&r, 0, 0, 8, 1, 4, 0x2000, 0);
		if (m[i].mode == EPD_UI_SPEED) {
			CHECK_EQ(step(&r, 5000, &wait), EPD_UI_IDLE);
		} else {
			CHECK_EQ(step(&r, 5000, &wait), EPD_UI_SEND);
			EXPECT_REQ(&r, 0, 0, 8, 1, m[i].wf, m[i].flag, 1);
		}
		epd_ui_release(&r.ui);
	}
}

TEST(ui_long_bursts_never_get_gc16)
{
	struct ui_rig r;
	u64 wait = 0, t = 1000;
	int i;

	ui_rig_init(&r, EPD_UI_SPEED);
	CHECK_EQ(frame(&r, 0), 1);
	r.cfg.big_pct = 50;
	for (i = 0; i < 10; i++, t += 50) {
		poke(&r, 0, 0);
		CHECK_EQ(frame(&r, t), 1);
	}
	poke_rows(&r, 0, H);
	CHECK_EQ(frame(&r, t), 1);
	CHECK_EQ(step(&r, t + 5000, &wait), EPD_UI_IDLE);
	epd_ui_release(&r.ui);
}

TEST(ui_unreadable_frame_updates_everything_untracked)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_SPEED);
	r.cfg.big_pct = 50;
	memset(r.req, 0xee, sizeof(r.req));
	r.n = epd_ui_frame(&r.ui, &r.cfg, r.fb, W * 4 - 1, LEN, 0, r.req, &r.cut);
	EXPECT_REQ(&r, 0, 0, W, H, 4, 0x2000, 0);
	r.n = epd_ui_frame(&r.ui, &r.cfg, r.fb, PITCH, LEN - 1, 100, r.req, &r.cut);
	EXPECT_REQ(&r, 0, 0, W, H, 4, 0x2000, 0);
	CHECK_EQ(step(&r, 5000, &wait), EPD_UI_IDLE);
	epd_ui_release(&r.ui);
}

TEST(ui_invalidate_forgets_the_last_frame)
{
	struct ui_rig r;

	ui_rig_init(&r, EPD_UI_SPEED);
	CHECK_EQ(frame(&r, 0), 1);
	CHECK_EQ(frame(&r, 1000), 0);
	epd_ui_invalidate(&r.ui);
	CHECK_EQ(frame(&r, 2000), 1);
	EXPECT_REQ(&r, 0, 0, W, H, 4, 0x2000, 0);
	epd_ui_release(&r.ui);
}

TEST(ui_reads_only_the_visible_width)
{
	struct ui_rig r;
	u8 *exact = malloc((H - 1) * PITCH + W * 4);

	ui_rig_init(&r, EPD_UI_SPEED);
	memset(exact, 0, (H - 1) * PITCH + W * 4);
	CHECK_EQ(epd_ui_frame(&r.ui, &r.cfg, exact, PITCH, LEN, 0, r.req, &r.cut), 1);
	exact[(H - 1) * PITCH + W * 4 - 1] = 1;
	r.n = epd_ui_frame(&r.ui, &r.cfg, exact, PITCH, LEN, 1000, r.req, &r.cut);
	EXPECT_REQ(&r, 24, H - 1, 8, 1, 4, 0x2000, 0);
	free(exact);
	epd_ui_release(&r.ui);
}

TEST(ui_gc_request_is_a_full_gc16_without_a_marker)
{
	struct epd_request q;

	memset(&q, 0x5a, sizeof(q));
	epd_ui_gc_request(&q, W, H);
	CHECK_EQ(q.left, 0);
	CHECK_EQ(q.top, 0);
	CHECK_EQ(q.width, W);
	CHECK_EQ(q.height, H);
	CHECK_EQ(q.mode, 2);
	CHECK_EQ(q.flags, 0x1000);
	CHECK_EQ(q.update_mode, 1);
	CHECK_EQ(q.marker, 0);
}

TEST(ui_separate_changes_get_their_own_strips)
{
	struct ui_rig r;

	ui_rig_init(&r, EPD_UI_SPEED);
	CHECK_EQ(frame(&r, 0), 1);
	poke(&r, 13, 1);
	poke(&r, 2, 9);
	poke(&r, 21, 9);
	CHECK_EQ(frame(&r, 1000), 2);
	EXPECT_RECT(&r, 0, 8, 1, 8, 1, 4, 0x2000, 0);
	EXPECT_RECT(&r, 1, 0, 9, 24, 1, 4, 0x2000, 0);
	CHECK_EQ(r.req[0].marker, 2);
	CHECK_EQ(r.req[1].marker, 2);
	epd_ui_release(&r.ui);
}

TEST(ui_adjacent_rows_form_one_strip)
{
	struct ui_rig r;

	ui_rig_init(&r, EPD_UI_SPEED);
	CHECK_EQ(frame(&r, 0), 1);
	poke(&r, 3, 4);
	poke(&r, 17, 5);
	poke(&r, 9, 6);
	CHECK_EQ(frame(&r, 1000), 1);
	EXPECT_REQ(&r, 0, 4, 24, 3, 4, 0x2000, 0);
	epd_ui_release(&r.ui);
}

TEST(ui_extra_strips_merge_where_it_adds_least_area)
{
	struct ui_rig r;

	ui_rig_init(&r, EPD_UI_SPEED);
	CHECK_EQ(frame(&r, 0), 1);
	poke_rows(&r, 0, 1);
	poke_rows(&r, 2, 3);
	poke_span(&r, 0, 4, 4);
	poke_span(&r, 0, 4, 6);
	poke_span(&r, 28, 32, 8);
	poke_span(&r, 28, 32, 10);
	CHECK_EQ(frame(&r, 1000), 4);
	EXPECT_RECT(&r, 0, 0, 0, W, 1, 4, 0x2000, 0);
	EXPECT_RECT(&r, 1, 0, 2, W, 1, 4, 0x2000, 0);
	EXPECT_RECT(&r, 2, 0, 4, 8, 3, 4, 0x2000, 0);
	EXPECT_RECT(&r, 3, 24, 8, 8, 3, 4, 0x2000, 0);
	epd_ui_release(&r.ui);
}

TEST(ui_equal_merge_cost_joins_the_upper_pair)
{
	struct ui_rig r;
	u32 y;

	ui_rig_init(&r, EPD_UI_SPEED);
	CHECK_EQ(frame(&r, 0), 1);
	for (y = 0; y < 12; y += 2)
		poke_rows(&r, y, y + 1);
	CHECK_EQ(frame(&r, 1000), 4);
	EXPECT_RECT(&r, 0, 0, 0, W, 5, 4, 0x2000, 0);
	EXPECT_RECT(&r, 1, 0, 6, W, 1, 4, 0x2000, 0);
	EXPECT_RECT(&r, 2, 0, 8, W, 1, 4, 0x2000, 0);
	EXPECT_RECT(&r, 3, 0, 10, W, 1, 4, 0x2000, 0);
	epd_ui_release(&r.ui);
}

TEST(ui_settle_covers_only_the_transient_rows)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_NORMAL);
	CHECK_EQ(frame(&r, 0), 1);
	poke_rows(&r, 12, 13);
	CHECK_EQ(frame(&r, 1000), 1);
	EXPECT_REQ(&r, 0, 12, W, 1, 255, 0, 0);
	poke(&r, 9, 3);
	CHECK_EQ(frame(&r, 1050), 1);
	EXPECT_REQ(&r, 8, 3, 8, 1, 4, 0x2000, 0);
	CHECK_EQ(step(&r, 1200, &wait), EPD_UI_SEND);
	EXPECT_REQ(&r, 8, 3, 8, 1, 255, 0, 1);
	CHECK_EQ(step(&r, 1300, &wait), EPD_UI_IDLE);
	CHECK_EQ(r.n, 0);
	epd_ui_release(&r.ui);
}

TEST(ui_big_burst_pass_also_covers_unsettled_transient_rows)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_NORMAL);
	CHECK_EQ(frame(&r, 0), 1);
	poke(&r, 1, 1);
	CHECK_EQ(frame(&r, 1000), 1);
	poke(&r, 9, 12);
	CHECK_EQ(frame(&r, 1050), 1);
	r.cfg.big_pct = 50;
	poke_rows(&r, 0, 8);
	CHECK_EQ(frame(&r, 1500), 1);
	EXPECT_REQ(&r, 0, 0, W, 8, 255, 0, 0);
	CHECK_EQ(step(&r, 1899, &wait), EPD_UI_WAIT);
	CHECK_EQ(step(&r, 1900, &wait), EPD_UI_SEND);
	CHECK_EQ(r.n, 2);
	EXPECT_RECT(&r, 0, 0, 0, W, 8, 2, 0x1000, 1);
	EXPECT_RECT(&r, 1, 8, 12, 8, 1, 2, 0x1000, 1);
	CHECK_EQ(step(&r, 5000, &wait), EPD_UI_IDLE);
	epd_ui_release(&r.ui);
}

TEST(ui_unreadable_frame_makes_the_next_pass_full_screen)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_NORMAL);
	CHECK_EQ(frame(&r, 0), 1);
	poke(&r, 0, 0);
	CHECK_EQ(frame(&r, 1000), 1);
	r.n = epd_ui_frame(&r.ui, &r.cfg, r.fb, W * 4 - 1, LEN, 1050, r.req, &r.cut);
	EXPECT_REQ(&r, 0, 0, W, H, 4, 0x2000, 0);
	CHECK_EQ(step(&r, 1200, &wait), EPD_UI_SEND);
	EXPECT_REQ(&r, 0, 0, W, H, 255, 0, 1);

	poke(&r, 0, 0);
	CHECK_EQ(frame(&r, 2000), 1);
	poke(&r, 9, 5);
	CHECK_EQ(frame(&r, 2050), 1);
	CHECK_EQ(step(&r, 2200, &wait), EPD_UI_SEND);
	EXPECT_REQ(&r, 8, 5, 8, 1, 255, 0, 1);
	epd_ui_release(&r.ui);
}

TEST(ui_gc16_frame_clears_the_transient_rows)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_NORMAL);
	CHECK_EQ(frame(&r, 0), 1);
	poke(&r, 0, 0);
	CHECK_EQ(frame(&r, 1000), 1);
	poke(&r, 9, 12);
	CHECK_EQ(frame(&r, 1050), 1);
	r.cfg.mode = EPD_UI_REGAL;
	poke(&r, 1, 1);
	CHECK_EQ(frame(&r, 1080), 1);
	EXPECT_REQ(&r, 0, 0, W, H, 2, 0x1000, 1);
	CHECK_EQ(step(&r, 1500, &wait), EPD_UI_IDLE);
	poke(&r, 1, 3);
	CHECK_EQ(frame(&r, 2000), 1);
	poke(&r, 17, 5);
	CHECK_EQ(frame(&r, 2050), 1);
	CHECK_EQ(step(&r, 2200, &wait), EPD_UI_SEND);
	EXPECT_REQ(&r, 16, 5, 8, 1, 5, 0x1000, 1);
	epd_ui_release(&r.ui);
}

TEST(ui_flash_waits_for_quiet_frames)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_SPEED);
	CHECK_EQ(frame(&r, 1000), 1);
	CHECK_EQ(epd_ui_flash(&r.ui, 1010), 0);
	CHECK_EQ(step(&r, 1010, &wait), EPD_UI_WAIT);
	CHECK_EQ(wait, 140);
	poke(&r, 0, 0);
	CHECK_EQ(frame(&r, 1100), 1);
	CHECK_EQ(step(&r, 1249, &wait), EPD_UI_WAIT);
	CHECK_EQ(wait, 1);
	CHECK_EQ(step(&r, 1250, &wait), EPD_UI_SEND);
	EXPECT_REQ(&r, 0, 0, W, H, 2, 0x1000, 1);
	CHECK_EQ(r.ui.flashes, 1);
	CHECK_EQ(step(&r, 1251, &wait), EPD_UI_IDLE);
	epd_ui_release(&r.ui);
}

TEST(ui_flash_without_a_frame_fires_at_once)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_SPEED);
	CHECK_EQ(epd_ui_flash(&r.ui, 0), 0);
	CHECK_EQ(step(&r, 0, &wait), EPD_UI_SEND);
	EXPECT_REQ(&r, 0, 0, W, H, 2, 0x1000, 1);
	epd_ui_release(&r.ui);
}

TEST(ui_flash_absorbs_repeats_and_drops_early_requests)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_SPEED);
	CHECK_EQ(frame(&r, 0), 1);
	CHECK_EQ(epd_ui_flash(&r.ui, 1000), 0);
	CHECK_EQ(epd_ui_flash(&r.ui, 1001), 0);
	CHECK_EQ(step(&r, 1001, &wait), EPD_UI_SEND);
	CHECK_EQ(step(&r, 1002, &wait), EPD_UI_IDLE);
	CHECK_EQ(epd_ui_flash(&r.ui, 1500), -EBUSY);
	CHECK_EQ(r.ui.flash_drops, 1);
	CHECK_EQ(step(&r, 1500, &wait), EPD_UI_IDLE);
	CHECK_EQ(epd_ui_flash(&r.ui, 1501), 0);
	CHECK_EQ(step(&r, 1501, &wait), EPD_UI_SEND);
	CHECK_EQ(r.ui.flashes, 2);
	CHECK_EQ(r.ui.flash_drops, 1);
	epd_ui_release(&r.ui);
}

TEST(ui_flash_replaces_a_pending_big_burst)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_SPEED);
	r.cfg.big_pct = 50;
	CHECK_EQ(frame(&r, 0), 1);
	CHECK_EQ(epd_ui_flash(&r.ui, 100), 0);
	CHECK_EQ(step(&r, 149, &wait), EPD_UI_WAIT);
	CHECK_EQ(wait, 1);
	CHECK_EQ(step(&r, 150, &wait), EPD_UI_SEND);
	EXPECT_REQ(&r, 0, 0, W, H, 2, 0x1000, 1);
	CHECK_EQ(step(&r, 5000, &wait), EPD_UI_IDLE);
	epd_ui_release(&r.ui);
}

TEST(ui_flash_starts_a_new_burst)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_SPEED);
	r.cfg.big_pct = 50;
	CHECK_EQ(frame(&r, 0), 1);
	CHECK_EQ(epd_ui_flash(&r.ui, 150), 0);
	CHECK_EQ(step(&r, 150, &wait), EPD_UI_SEND);
	poke(&r, 0, 0);
	CHECK_EQ(frame(&r, 300), 1);
	CHECK_EQ(step(&r, 5000, &wait), EPD_UI_IDLE);
	epd_ui_release(&r.ui);
}

TEST(ui_frame_gc16_serves_a_pending_flash)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_SPEED);
	CHECK_EQ(frame(&r, 0), 1);
	CHECK_EQ(epd_ui_flash(&r.ui, 50), 0);
	r.cfg.mode = EPD_UI_REGAL;
	poke(&r, 0, 0);
	CHECK_EQ(frame(&r, 100), 1);
	EXPECT_REQ(&r, 0, 0, W, H, 2, 0x1000, 1);
	CHECK_EQ(r.ui.flashes, 1);
	CHECK_EQ(step(&r, 5000, &wait), EPD_UI_IDLE);
	CHECK_EQ(epd_ui_flash(&r.ui, 599), -EBUSY);
	epd_ui_release(&r.ui);
}

TEST(ui_flash_restarts_the_gc_interval)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_SPEED);
	r.cfg.gc_interval = 2;
	CHECK_EQ(frame(&r, 0), 1);
	poke(&r, 0, 0);
	CHECK_EQ(frame(&r, 1000), 1);
	EXPECT_REQ(&r, 0, 0, 8, 1, 4, 0x2000, 0);
	CHECK_EQ(epd_ui_flash(&r.ui, 1000), 0);
	CHECK_EQ(step(&r, 1150, &wait), EPD_UI_SEND);
	poke(&r, 0, 0);
	CHECK_EQ(frame(&r, 2000), 1);
	EXPECT_REQ(&r, 0, 0, 8, 1, 4, 0x2000, 0);
	poke(&r, 0, 0);
	CHECK_EQ(frame(&r, 3000), 1);
	EXPECT_REQ(&r, 0, 0, 8, 1, 4, 0x2000, 0);
	poke(&r, 0, 0);
	CHECK_EQ(frame(&r, 4000), 1);
	EXPECT_REQ(&r, 0, 0, 8, 1, 2, 0x1000, 1);
	epd_ui_release(&r.ui);
}

TEST(ui_flash_replaces_a_pending_settle_pass)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_NORMAL);
	CHECK_EQ(frame(&r, 0), 1);
	poke(&r, 9, 1);
	CHECK_EQ(frame(&r, 99), 1);
	EXPECT_REQ(&r, 8, 1, 8, 1, 4, 0x2000, 0);
	CHECK_EQ(epd_ui_flash(&r.ui, 99), 0);
	CHECK_EQ(step(&r, 248, &wait), EPD_UI_WAIT);
	CHECK_EQ(wait, 1);
	CHECK_EQ(step(&r, 249, &wait), EPD_UI_SEND);
	EXPECT_REQ(&r, 0, 0, W, H, 2, 0x1000, 1);
	CHECK_EQ(step(&r, 5000, &wait), EPD_UI_IDLE);
	epd_ui_release(&r.ui);
}

TEST(ui_seed_takes_the_frame_as_shown)
{
	struct ui_rig r;

	ui_rig_init(&r, EPD_UI_SPEED);
	CHECK_EQ(frame(&r, 0), 1);
	poke_rows(&r, 0, H);
	CHECK_EQ(epd_ui_seed(&r.ui, r.fb, PITCH, LEN, 500), 0);
	CHECK_EQ(frame(&r, 1000), 0);
	poke(&r, 9, 1);
	CHECK_EQ(frame(&r, 2000), 1);
	EXPECT_REQ(&r, 8, 1, 8, 1, 4, 0x2000, 0);
	epd_ui_release(&r.ui);
}

TEST(ui_seed_cancels_pending_passes)
{
	struct ui_rig r, q;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_SPEED);
	r.cfg.big_pct = 50;
	CHECK_EQ(frame(&r, 0), 1);
	CHECK_EQ(epd_ui_seed(&r.ui, r.fb, PITCH, LEN, 1), 0);
	CHECK_EQ(step(&r, 5000, &wait), EPD_UI_IDLE);
	epd_ui_release(&r.ui);

	ui_rig_init(&q, EPD_UI_NORMAL);
	CHECK_EQ(frame(&q, 0), 1);
	poke(&q, 9, 1);
	CHECK_EQ(frame(&q, 99), 1);
	CHECK_EQ(epd_ui_seed(&q.ui, q.fb, PITCH, LEN, 100), 0);
	CHECK_EQ(step(&q, 5000, &wait), EPD_UI_IDLE);
	epd_ui_release(&q.ui);
}

TEST(ui_seed_restarts_the_gc_interval)
{
	struct ui_rig r;

	ui_rig_init(&r, EPD_UI_SPEED);
	r.cfg.gc_interval = 2;
	CHECK_EQ(frame(&r, 0), 1);
	poke(&r, 0, 0);
	CHECK_EQ(frame(&r, 500), 1);
	EXPECT_REQ(&r, 0, 0, 8, 1, 4, 0x2000, 0);
	CHECK_EQ(epd_ui_seed(&r.ui, r.fb, PITCH, LEN, 750), 0);
	poke(&r, 0, 0);
	CHECK_EQ(frame(&r, 1000), 1);
	EXPECT_REQ(&r, 0, 0, 8, 1, 4, 0x2000, 0);
	poke(&r, 0, 0);
	CHECK_EQ(frame(&r, 1500), 1);
	EXPECT_REQ(&r, 0, 0, 8, 1, 4, 0x2000, 0);
	poke(&r, 0, 0);
	CHECK_EQ(frame(&r, 2000), 1);
	EXPECT_REQ(&r, 0, 0, 8, 1, 2, 0x1000, 1);
	epd_ui_release(&r.ui);
}

TEST(ui_seed_rejects_an_unreadable_frame)
{
	struct ui_rig r;

	ui_rig_init(&r, EPD_UI_SPEED);
	CHECK_EQ(frame(&r, 0), 1);
	CHECK_EQ(epd_ui_seed(&r.ui, r.fb, W * 4 - 1, LEN, 500), -EINVAL);
	CHECK_EQ(epd_ui_seed(&r.ui, r.fb, PITCH, PITCH * H - 1, 500), -EINVAL);
	CHECK_EQ(frame(&r, 1000), 1);
	EXPECT_REQ(&r, 0, 0, W, H, 4, 0x2000, 0);
	epd_ui_release(&r.ui);
}

TEST(ui_seed_unreadable_still_cancels_a_pending_big_pass)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_SPEED);
	r.cfg.big_pct = 50;
	CHECK_EQ(frame(&r, 0), 1);
	CHECK(r.ui.big_pending);
	CHECK_EQ(epd_ui_seed(&r.ui, r.fb, W * 4 - 1, LEN, 500), -EINVAL);
	CHECK_EQ(step(&r, 5000, &wait), EPD_UI_IDLE);
	epd_ui_release(&r.ui);
}

TEST(ui_seed_serves_a_pending_flash)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_SPEED);
	CHECK_EQ(frame(&r, 0), 1);
	CHECK_EQ(epd_ui_flash(&r.ui, 50), 0);
	CHECK_EQ(epd_ui_seed(&r.ui, r.fb, PITCH, LEN, 100), 0);
	CHECK_EQ(r.ui.flashes, 1);
	CHECK_EQ(step(&r, 5000, &wait), EPD_UI_IDLE);
	CHECK_EQ(epd_ui_flash(&r.ui, 599), -EBUSY);
	epd_ui_release(&r.ui);
}

TEST(ui_only_night_inverts)
{
	int m;

	for (m = 0; m < EPD_UI_NMODES; m++)
		CHECK_EQ(epd_ui_mode_inverted(m), m == EPD_UI_NIGHT);
	CHECK(!epd_ui_mode_inverted(-1));
	CHECK(!epd_ui_mode_inverted(EPD_UI_NMODES));
}

TEST(ui_entering_night_refreshes_with_gc16)
{
	struct ui_rig r;

	ui_rig_init(&r, EPD_UI_SPEED);
	CHECK_EQ(frame(&r, 0), 1);
	r.cfg.mode = EPD_UI_NIGHT;
	poke(&r, 0, 0);
	CHECK_EQ(frame(&r, 1000), 1);
	EXPECT_REQ(&r, 0, 0, W, H, 2, 0x1000, 1);
	CHECK_EQ(r.cut, 0);
	epd_ui_release(&r.ui);
}
