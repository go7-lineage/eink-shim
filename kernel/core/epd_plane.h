/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef EPD_PLANE_H
#define EPD_PLANE_H

#include "epd_policy.h"

#define EPD_PLANE_WARM	16

struct epd_plane {
	u32 w, h;
	u8 *grey;
	u8 *in;
	u8 *out;
	int *err;
	struct epd_policy pol;
	struct epd_queue q;
};

int epd_plane_init(struct epd_plane *pl, u32 w, u32 h, bool du_a2_clean);
void epd_plane_release(struct epd_plane *pl);
void epd_plane_push(struct epd_plane *pl, const struct epd_request *r, int n);
bool epd_plane_pop(struct epd_plane *pl, struct epd_engine *e, u32 conv, struct epd_decision *d, int *ret);

#endif
