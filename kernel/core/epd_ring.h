/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef EPD_RING_H
#define EPD_RING_H

#include "epd_port.h"

#define EPD_RING_BUFS	3

struct epd_ring {
	int shown, inflight, pending, filling;
	bool link_req;
};

void epd_ring_init(struct epd_ring *r);
bool epd_ring_idle(const struct epd_ring *r);
bool epd_ring_can_fill(const struct epd_ring *r);
int epd_ring_begin_fill(struct epd_ring *r);
void epd_ring_end_fill(struct epd_ring *r);
bool epd_ring_has_work(const struct epd_ring *r);
int epd_ring_take(struct epd_ring *r, bool link_on, bool *null);
void epd_ring_done(struct epd_ring *r);

#endif
