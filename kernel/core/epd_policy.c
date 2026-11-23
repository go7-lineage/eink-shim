// SPDX-License-Identifier: GPL-2.0-only
/*
 * Update policy and queue between userspace requests and engine.
 */
#include "epd_policy.h"

#define M_DU	1
#define M_GC16	2
#define M_A2	4
#define M_AUTO	0xff

bool epd_request_clip(struct epd_request *r, s32 left, s32 top, s32 width, s32 height, u32 w, u32 h)
{
	s64 x0 = left > 0 ? left : 0, y0 = top > 0 ? top : 0;
	s64 x1 = (s64)left + width, y1 = (s64)top + height;

	x1 = x1 < (s64)w ? x1 : (s64)w;
	y1 = y1 < (s64)h ? y1 : (s64)h;
	if (x0 >= x1 || y0 >= y1)
		return false;
	r->left = (u16)x0;
	r->top = (u16)y0;
	r->width = (u16)(x1 - x0);
	r->height = (u16)(y1 - y0);
	return true;
}

void epd_policy_init(struct epd_policy *p, bool du_a2_clean)
{
	memset(p, 0, sizeof(*p));
	p->last_mode = -1;
	p->du_a2_clean = du_a2_clean;
}

static bool is_regal(int m)
{
	return epd_mode_is_regal(m);
}

void epd_policy_apply(struct epd_policy *p, const struct epd_request *in, struct epd_decision *out)
{
	int mode = in->mode, last = p->last_mode;
	bool full = in->update_mode == 1, barrier;

	barrier = (last == M_DU && mode != M_DU) ||
		  (last == M_GC16 && !(p->last_flags & 8) && (mode == M_DU || mode == M_A2)) ||
		  (mode == M_GC16 && !(in->flags & 8) && (last == M_DU || last == M_A2)) ||
		  (p->du_a2_clean && mode != M_A2 && last == M_A2);
	if (mode == M_AUTO)
		mode = M_GC16;

	if (mode == M_A2 || (in->flags & 0x8000))
		full = true;
	if ((last == M_A2 && mode != M_A2) || (is_regal(last) && !is_regal(mode)) ||
	    (last == M_DU && mode != M_DU))
		full = true;
	if (p->du_a2_clean && mode == M_A2 && last != M_A2 && last != -1) {
		mode = M_DU;
		full = true;
	}

	out->upd.top = in->top;
	out->upd.left = in->left;
	out->upd.width = in->width;
	out->upd.height = in->height;
	out->upd.mode = mode;
	out->upd.full = full;
	out->upd.marker = in->marker;
	out->flags = in->flags;
	out->barrier = barrier;

	p->last_mode = in->mode == M_AUTO ? M_GC16 : in->mode;
	p->last_flags = in->flags;
}

void epd_queue_init(struct epd_queue *q)
{
	memset(q, 0, sizeof(*q));
}

static bool queue_same_kind(const struct epd_decision *t, const struct epd_decision *d)
{
	return !d->barrier && t->upd.mode == d->upd.mode && t->upd.full == d->upd.full && t->flags == d->flags &&
	       t->upd.left < d->upd.left + d->upd.width && d->upd.left < t->upd.left + t->upd.width &&
	       t->upd.top < d->upd.top + d->upd.height && d->upd.top < t->upd.top + t->upd.height;
}

void epd_queue_push(struct epd_queue *q, const struct epd_decision *d)
{
	struct epd_decision *t = &q->q[(q->head + q->n + EPD_QMAX - 1) % EPD_QMAX];
	u32 tr = (u32)t->upd.left + t->upd.width, dr = (u32)d->upd.left + d->upd.width;
	u32 tb = (u32)t->upd.top + t->upd.height, db = (u32)d->upd.top + d->upd.height;
	u32 r = tr > dr ? tr : dr, b = tb > db ? tb : db;

	if (q->n < EPD_QMAX && !(q->merge && q->n && queue_same_kind(t, d))) {
		q->q[(q->head + q->n++) % EPD_QMAX] = *d;
		return;
	}
	t->upd.left = t->upd.left < d->upd.left ? t->upd.left : d->upd.left;
	t->upd.top = t->upd.top < d->upd.top ? t->upd.top : d->upd.top;
	t->upd.width = (u16)(r - t->upd.left);
	t->upd.height = (u16)(b - t->upd.top);
	t->upd.mode = d->upd.mode;
	t->upd.full |= d->upd.full;
	t->upd.marker = d->upd.marker;
	t->flags = d->flags;
	t->barrier |= d->barrier;
	q->coalesced++;
}

bool epd_queue_pop(struct epd_queue *q, bool engine_busy, struct epd_decision *out)
{
	struct epd_decision *h = &q->q[q->head];

	if (!q->n || (h->barrier && engine_busy))
		return false;
	*out = *h;
	q->head = (q->head + 1) % EPD_QMAX;
	q->n--;
	return true;
}
