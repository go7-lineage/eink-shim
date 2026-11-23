// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>
#include <string.h>

#include "epd_ui.h"
#include "test.h"

#define W	32
#define H	8
#define PITCH	(W * 4 + 16)
#define LEN	(PITCH * H)

struct ui_rig {
	struct epd_ui ui;
	struct epd_ui_cfg cfg;
	u8 fb[LEN];
	struct epd_request req;
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

static int frame(struct ui_rig *r, u64 now)
{
	memset(&r->req, 0xee, sizeof(r->req));
	r->cut = 1234;
	return epd_ui_frame(&r->ui, &r->cfg, r->fb, PITCH, LEN, now, &r->req, &r->cut);
}

#define EXPECT_REQ(r, x, y, w, h, wf, fl, um)		\
	do {						\
		CHECK_EQ((r)->req.left, x);		\
		CHECK_EQ((r)->req.top, y);		\
		CHECK_EQ((r)->req.width, w);		\
		CHECK_EQ((r)->req.height, h);		\
		CHECK_EQ((r)->req.mode, wf);		\
		CHECK_EQ((r)->req.flags, fl);		\
		CHECK_EQ((r)->req.update_mode, um);	\
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
	CHECK_EQ(epd_ui_mode_by_name("fast"), -EINVAL);
	CHECK_EQ(epd_ui_mode_by_name(""), -EINVAL);
	CHECK_EQ(epd_ui_mode_by_name("speed "), -EINVAL);
	CHECK(!strcmp(epd_ui_mode_name(EPD_UI_SPEED), "speed"));
	CHECK(!strcmp(epd_ui_mode_name(EPD_UI_REGALPLUS), "regalplus"));
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
	CHECK_EQ(r.req.marker, 1);
	CHECK_EQ(r.cut, 3);
	CHECK_EQ(epd_ui_step(&r.ui, 1000, &r.req, &wait), EPD_UI_IDLE);
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
	CHECK_EQ(r.req.marker, 2);
	CHECK_EQ(r.cut, -1);
	poke(&r, 3, 2);
	poke(&r, 29, 6);
	CHECK_EQ(frame(&r, 2000), 1);
	EXPECT_REQ(&r, 0, 2, W, 5, 4, 0x2000, 0);
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
	struct epd_request req;
	struct epd_ui u;
	int cut;

	CHECK_EQ(epd_ui_init(&u, W, H), 0);
	CHECK_EQ(epd_ui_frame(&u, &cfg, fb, W * 4, sizeof(fb), 0, &req, &cut), 1);
	CHECK_EQ(epd_ui_frame(&u, &cfg, fb, W * 4, sizeof(fb), 1000, &req, &cut), 0);
	epd_ui_release(&u);
}

TEST(ui_modes_pick_waveform_and_flags)
{
	static const struct { int mode, wf; u32 flag; } want[] = {
		{ EPD_UI_SPEED, 4, 0x2000 }, { EPD_UI_NORMAL, 255, 0 },
		{ EPD_UI_REGAL, 5, 0x1000 }, { EPD_UI_REGALPLUS, 16, 0 }, { 99, 4, 0x2000 },
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

TEST(ui_debounced_mode_settles_after_a_burst)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_NORMAL);
	CHECK_EQ(frame(&r, 0), 1);
	EXPECT_REQ(&r, 0, 0, W, H, 255, 0, 0);
	CHECK_EQ(epd_ui_step(&r.ui, 10, &r.req, &wait), EPD_UI_IDLE);
	poke(&r, 9, 1);
	CHECK_EQ(frame(&r, 99), 1);
	EXPECT_REQ(&r, 8, 1, 8, 1, 4, 0x2000, 0);
	CHECK_EQ(epd_ui_step(&r.ui, 149, &r.req, &wait), EPD_UI_WAIT);
	CHECK_EQ(wait, 100);
	CHECK_EQ(epd_ui_step(&r.ui, 248, &r.req, &wait), EPD_UI_WAIT);
	CHECK_EQ(wait, 1);
	CHECK_EQ(epd_ui_step(&r.ui, 249, &r.req, &wait), EPD_UI_SEND);
	EXPECT_REQ(&r, 0, 0, W, H, 255, 0, 1);
	CHECK_EQ(r.req.marker, 3);
	CHECK_EQ(epd_ui_step(&r.ui, 250, &r.req, &wait), EPD_UI_IDLE);
	poke(&r, 9, 1);
	CHECK_EQ(frame(&r, 349), 1);
	EXPECT_REQ(&r, 8, 1, 8, 1, 255, 0, 0);
	CHECK_EQ(epd_ui_step(&r.ui, 350, &r.req, &wait), EPD_UI_IDLE);
	epd_ui_release(&r.ui);
}

TEST(ui_big_burst_gets_gc16_after_quiet)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_SPEED);
	r.cfg.big_pct = 50;
	CHECK_EQ(frame(&r, 0), 1);
	CHECK_EQ(epd_ui_step(&r.ui, 399, &r.req, &wait), EPD_UI_WAIT);
	CHECK_EQ(wait, 1);
	CHECK_EQ(epd_ui_step(&r.ui, 400, &r.req, &wait), EPD_UI_SEND);
	EXPECT_REQ(&r, 0, 0, W, H, 2, 0x1000, 1);
	CHECK_EQ(epd_ui_step(&r.ui, 401, &r.req, &wait), EPD_UI_IDLE);

	poke_rows(&r, 0, 3);
	CHECK_EQ(frame(&r, 1000), 1);
	CHECK_EQ(epd_ui_step(&r.ui, 2000, &r.req, &wait), EPD_UI_IDLE);
	poke_rows(&r, 3, 6);
	CHECK_EQ(frame(&r, 1399), 1);
	CHECK_EQ(epd_ui_step(&r.ui, 1500, &r.req, &wait), EPD_UI_WAIT);
	CHECK_EQ(epd_ui_step(&r.ui, 1799, &r.req, &wait), EPD_UI_SEND);
	EXPECT_REQ(&r, 0, 0, W, H, 2, 0x1000, 1);

	poke_rows(&r, 0, 3);
	CHECK_EQ(frame(&r, 3000), 1);
	poke_rows(&r, 3, 6);
	CHECK_EQ(frame(&r, 3400), 1);
	CHECK_EQ(epd_ui_step(&r.ui, 5000, &r.req, &wait), EPD_UI_IDLE);
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
	poke_rows(&r, 0, 3);
	for (x = 0; x < 30; x++)
		poke(&r, x, 3);
	CHECK_EQ(frame(&r, 1000), 1);
	CHECK_EQ(epd_ui_step(&r.ui, 5000, &r.req, &wait), EPD_UI_IDLE);
	poke_rows(&r, 0, 4);
	CHECK_EQ(frame(&r, 6000), 1);
	CHECK_EQ(epd_ui_step(&r.ui, 9000, &r.req, &wait), EPD_UI_SEND);
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
	CHECK_EQ(epd_ui_step(&r.ui, t + 5000, &r.req, &wait), EPD_UI_SEND);
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
	CHECK_EQ(epd_ui_step(&r.ui, 5000, &r.req, &wait), EPD_UI_IDLE);
	epd_ui_release(&r.ui);
}

TEST(ui_regal_modes_are_debounced)
{
	static const struct { int mode, wf; u32 flag; } m[] = {
		{ EPD_UI_REGAL, 5, 0x1000 }, { EPD_UI_REGALPLUS, 16, 0 }, { EPD_UI_SPEED, 4, 0x2000 },
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
			CHECK_EQ(epd_ui_step(&r.ui, 5000, &r.req, &wait), EPD_UI_IDLE);
		} else {
			CHECK_EQ(epd_ui_step(&r.ui, 5000, &r.req, &wait), EPD_UI_SEND);
			EXPECT_REQ(&r, 0, 0, W, H, m[i].wf, m[i].flag, 1);
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
	CHECK_EQ(epd_ui_step(&r.ui, t + 5000, &r.req, &wait), EPD_UI_IDLE);
	epd_ui_release(&r.ui);
}

TEST(ui_unreadable_frame_updates_everything_untracked)
{
	struct ui_rig r;
	u64 wait = 0;

	ui_rig_init(&r, EPD_UI_SPEED);
	r.cfg.big_pct = 50;
	memset(&r.req, 0xee, sizeof(r.req));
	CHECK_EQ(epd_ui_frame(&r.ui, &r.cfg, r.fb, W * 4 - 1, LEN, 0, &r.req, &r.cut), 1);
	EXPECT_REQ(&r, 0, 0, W, H, 4, 0x2000, 0);
	CHECK_EQ(epd_ui_frame(&r.ui, &r.cfg, r.fb, PITCH, LEN - 1, 100, &r.req, &r.cut), 1);
	EXPECT_REQ(&r, 0, 0, W, H, 4, 0x2000, 0);
	CHECK_EQ(epd_ui_step(&r.ui, 5000, &r.req, &wait), EPD_UI_IDLE);
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
	CHECK_EQ(epd_ui_frame(&r.ui, &r.cfg, exact, PITCH, LEN, 0, &r.req, &r.cut), 1);
	exact[(H - 1) * PITCH + W * 4 - 1] = 1;
	CHECK_EQ(epd_ui_frame(&r.ui, &r.cfg, exact, PITCH, LEN, 1000, &r.req, &r.cut), 1);
	EXPECT_REQ(&r, 24, H - 1, 8, 1, 4, 0x2000, 0);
	free(exact);
	epd_ui_release(&r.ui);
}
