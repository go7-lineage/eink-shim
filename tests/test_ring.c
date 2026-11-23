// SPDX-License-Identifier: GPL-2.0-only
#include "epd_ring.h"
#include "test.h"

static void check_roles(const struct epd_ring *r)
{
	const int role[4] = { r->shown, r->inflight, r->pending, r->filling };
	int i, j;

	for (i = 0; i < 4; i++) {
		CHECK(role[i] >= -1 && role[i] < EPD_RING_BUFS);
		for (j = i + 1; j < 4; j++)
			CHECK(role[i] < 0 || role[i] != role[j]);
	}
}

TEST(ring_starts_idle)
{
	struct epd_ring r;
	bool null;

	epd_ring_init(&r);
	CHECK(epd_ring_idle(&r));
	CHECK(epd_ring_can_fill(&r));
	CHECK(!epd_ring_has_work(&r));
	CHECK_EQ(epd_ring_take(&r, true, &null), -1);
	CHECK(!null);
	CHECK(epd_ring_idle(&r));
}

TEST(ring_frame_goes_pending_inflight_shown)
{
	struct epd_ring r;
	bool null;
	int f;

	epd_ring_init(&r);
	f = epd_ring_begin_fill(&r);
	CHECK(f >= 0);
	CHECK(epd_ring_idle(&r));
	CHECK(!epd_ring_has_work(&r));
	epd_ring_end_fill(&r);
	CHECK(!epd_ring_idle(&r));
	CHECK(!epd_ring_can_fill(&r));
	CHECK(epd_ring_has_work(&r));
	CHECK_EQ(epd_ring_take(&r, true, &null), f);
	CHECK(!null);
	CHECK(epd_ring_can_fill(&r));
	CHECK(!epd_ring_idle(&r));
	epd_ring_done(&r);
	CHECK(epd_ring_idle(&r));
	CHECK_EQ(r.shown, f);
}

TEST(ring_link_request_sends_a_null_frame_only_when_the_link_is_off)
{
	struct epd_ring r;
	bool null;
	int c;

	epd_ring_init(&r);
	r.link_req = true;
	CHECK(epd_ring_has_work(&r));
	CHECK_EQ(epd_ring_take(&r, true, &null), -1);
	CHECK(!null);
	CHECK(!r.link_req);
	CHECK(epd_ring_idle(&r));

	r.link_req = true;
	c = epd_ring_take(&r, false, &null);
	CHECK(c >= 0);
	CHECK(null);
	CHECK(!epd_ring_idle(&r));
	epd_ring_done(&r);
	CHECK_EQ(r.shown, c);
}

TEST(ring_pending_frame_wins_over_a_link_request)
{
	struct epd_ring r;
	bool null;
	int f;

	epd_ring_init(&r);
	f = epd_ring_begin_fill(&r);
	epd_ring_end_fill(&r);
	r.link_req = true;
	CHECK_EQ(epd_ring_take(&r, false, &null), f);
	CHECK(!null);
	CHECK(r.link_req);
	CHECK(!epd_ring_has_work(&r));
	epd_ring_done(&r);
	CHECK(epd_ring_has_work(&r));
}

TEST(ring_random_interleavings_keep_roles_apart)
{
	unsigned int seed = 99;
	int round;

	for (round = 0; round < 2000; round++) {
		struct epd_ring r;
		int step, filled = 0, taken = 0, last = -1;
		bool link_on = false;

		epd_ring_init(&r);
		for (step = 0; step < 200; step++) {
			bool null;
			int c;

			switch (test_rand(&seed) % 5) {
			case 0:
				if (r.filling < 0 && epd_ring_can_fill(&r)) {
					c = epd_ring_begin_fill(&r);
					CHECK(c >= 0);
				}
				break;
			case 1:
				if (r.filling >= 0) {
					last = r.filling;
					epd_ring_end_fill(&r);
					filled++;
				}
				break;
			case 2:
				if (r.inflight < 0 && epd_ring_has_work(&r)) {
					bool had = r.pending >= 0;

					c = epd_ring_take(&r, link_on, &null);
					CHECK(c >= 0 || !null);
					if (had) {
						CHECK_EQ(c, last);
						CHECK(!null);
						taken++;
					}
				}
				break;
			case 3:
				if (r.inflight >= 0) {
					epd_ring_done(&r);
					link_on = true;
				}
				break;
			default:
				if (test_rand(&seed) % 4 == 0)
					link_on = false;
				r.link_req = true;
				break;
			}
			check_roles(&r);
		}
		CHECK(taken <= filled);
		CHECK(filled - taken <= 1);
	}
}
