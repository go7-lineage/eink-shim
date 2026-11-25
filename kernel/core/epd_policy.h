/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef EPD_POLICY_H
#define EPD_POLICY_H

#include "epd_engine.h"

struct epd_request {
	u16 top, left, width, height;
	int mode;
	int update_mode;
	u32 marker;
	u32 flags;
};

struct epd_decision {
	struct epd_update upd;
	u32 flags;
	bool barrier;
};

struct epd_policy {
	int last_mode;
	u32 last_flags;
	bool du_a2_clean;
};

#define EPD_QMAX	16

struct epd_queue {
	struct epd_decision q[EPD_QMAX];
	int head, n;
	u32 coalesced;
	bool merge;
};

bool epd_request_clip(struct epd_request *r, s32 left, s32 top, s32 width, s32 height, u32 w, u32 h);
void epd_policy_init(struct epd_policy *p, bool du_a2_clean);
void epd_policy_apply(struct epd_policy *p, const struct epd_request *in, struct epd_decision *out);
void epd_policy_apply_group(struct epd_policy *p, const struct epd_request *in, int n, struct epd_decision *out);
void epd_queue_init(struct epd_queue *q);
void epd_queue_push(struct epd_queue *q, const struct epd_decision *d);
bool epd_queue_pop(struct epd_queue *q, bool engine_busy, struct epd_decision *out);

#endif
