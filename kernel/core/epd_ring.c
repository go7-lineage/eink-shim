// SPDX-License-Identifier: GPL-2.0-only
#include "epd_ring.h"

void epd_ring_init(struct epd_ring *r)
{
	r->shown = -1;
	r->inflight = -1;
	r->pending = -1;
	r->filling = -1;
	r->link_req = false;
}

bool epd_ring_idle(const struct epd_ring *r)
{
	return r->pending < 0 && r->inflight < 0;
}

bool epd_ring_can_fill(const struct epd_ring *r)
{
	return r->pending < 0;
}

static int ring_free(const struct epd_ring *r)
{
	int f;

	for (f = 0; f < EPD_RING_BUFS; f++)
		if (f != r->shown && f != r->inflight && f != r->pending && f != r->filling)
			return f;
	return -1;
}

int epd_ring_begin_fill(struct epd_ring *r)
{
	r->filling = ring_free(r);
	return r->filling;
}

void epd_ring_end_fill(struct epd_ring *r)
{
	r->pending = r->filling;
	r->filling = -1;
}

bool epd_ring_has_work(const struct epd_ring *r)
{
	return r->pending >= 0 || (r->link_req && r->inflight < 0);
}

int epd_ring_take(struct epd_ring *r, bool link_on, bool *null)
{
	int c = r->pending;

	*null = false;
	if (c >= 0) {
		r->pending = -1;
	} else if (r->link_req) {
		r->link_req = false;
		if (!link_on) {
			c = ring_free(r);
			*null = c >= 0;
		}
	}
	r->inflight = c;
	return c;
}

void epd_ring_done(struct epd_ring *r)
{
	r->shown = r->inflight;
	r->inflight = -1;
}
