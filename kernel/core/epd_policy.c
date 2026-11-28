// SPDX-License-Identifier: GPL-2.0-only
/*
 * Update policy and queue between userspace requests and engine.
 */
#include "epd_policy.h"

#define M_DU	EPD_MODE_DU
#define M_GC16	EPD_MODE_GC16
#define M_A2	EPD_MODE_A2
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
	if (!in->no_clean && ((last == M_A2 && mode != M_A2) || (is_regal(last) && !is_regal(mode)) ||
			      (last == M_DU && mode != M_DU)))
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
	out->upd.repeat = mode == M_A2 && !in->once && !p->a2_once;
	out->upd.marker = in->marker;
	out->flags = in->flags;
	out->barrier = barrier;

	p->last_mode = in->mode == M_AUTO ? M_GC16 : in->mode;
	p->last_flags = in->flags;
}

void epd_policy_apply_next(const struct epd_decision *first, const struct epd_request *in, struct epd_decision *out)
{
	*out = *first;
	out->upd.top = in->top;
	out->upd.left = in->left;
	out->upd.width = in->width;
	out->upd.height = in->height;
	out->upd.marker = in->marker;
	out->barrier = false;
}

void epd_policy_apply_group(struct epd_policy *p, const struct epd_request *in, int n, struct epd_decision *out)
{
	int i;

	if (n < 1)
		return;
	epd_policy_apply(p, &in[0], &out[0]);
	for (i = 1; i < n; i++)
		epd_policy_apply_next(&out[0], &in[i], &out[i]);
}

void epd_queue_init(struct epd_queue *q)
{
	memset(q, 0, sizeof(*q));
}

struct queue_rect {
	u32 x0, y0, x1, y1;
};

static struct queue_rect queue_rect(const struct epd_decision *d)
{
	struct queue_rect r = { d->upd.left, d->upd.top, (u32)d->upd.left + d->upd.width,
				(u32)d->upd.top + d->upd.height };

	return r;
}

static u64 queue_area(const struct queue_rect *r)
{
	return r->x1 > r->x0 && r->y1 > r->y0 ? (u64)(r->x1 - r->x0) * (r->y1 - r->y0) : 0;
}

static struct queue_rect queue_box(const struct queue_rect *a, const struct queue_rect *b)
{
	struct queue_rect r = { a->x0 < b->x0 ? a->x0 : b->x0, a->y0 < b->y0 ? a->y0 : b->y0,
				a->x1 > b->x1 ? a->x1 : b->x1, a->y1 > b->y1 ? a->y1 : b->y1 };

	return r;
}

static void queue_set_rect(struct epd_decision *d, const struct queue_rect *r)
{
	d->upd.left = (u16)r->x0;
	d->upd.top = (u16)r->y0;
	d->upd.width = (u16)(r->x1 - r->x0);
	d->upd.height = (u16)(r->y1 - r->y0);
}

static struct epd_decision *queue_at(struct epd_queue *q, int i)
{
	return &q->q[(q->head + i) % EPD_QMAX];
}

static bool queue_same_kind(const struct epd_decision *t, const struct epd_decision *d)
{
	return t->upd.mode == d->upd.mode && t->upd.full == d->upd.full && t->upd.repeat == d->upd.repeat &&
	       t->flags == d->flags;
}

static bool queue_fast(const struct epd_decision *d)
{
	return d->upd.mode == M_A2 || d->upd.mode == M_DU;
}

static bool queue_join(struct epd_decision *t, const struct epd_decision *d)
{
	struct queue_rect a = queue_rect(t), b = queue_rect(d), box = queue_box(&a, &b);
	struct queue_rect both = { a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0,
				   a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1 };

	if (queue_area(&box) != queue_area(&a) + queue_area(&b) - queue_area(&both))
		return false;
	queue_set_rect(t, &box);
	t->upd.marker = d->upd.marker;
	return true;
}

static bool queue_drop_covered(struct epd_queue *q, const struct epd_decision *d)
{
	struct queue_rect outer = queue_rect(d);
	bool barrier = false;
	int i, k;

	for (i = 0; i < q->n;) {
		struct epd_decision *t = queue_at(q, i);
		struct queue_rect r = queue_rect(t);

		if (!queue_fast(t) || r.x0 < outer.x0 || r.y0 < outer.y0 || r.x1 > outer.x1 || r.y1 > outer.y1) {
			t->barrier |= barrier;
			barrier = false;
			i++;
			continue;
		}
		barrier |= t->barrier;
		for (k = i; k + 1 < q->n; k++)
			*queue_at(q, k) = *queue_at(q, k + 1);
		q->n--;
		q->coalesced++;
	}
	return barrier;
}

void epd_queue_push(struct epd_queue *q, const struct epd_decision *d)
{
	struct epd_decision in = *d, *t;
	struct queue_rect a, b, box;
	int i;

	if (q->merge && !queue_fast(&in))
		in.barrier |= queue_drop_covered(q, &in);
	for (i = q->n - 1; q->merge && !in.barrier && i >= 0; i--) {
		t = queue_at(q, i);
		if (!queue_same_kind(t, &in))
			break;
		if (queue_join(t, &in)) {
			q->coalesced++;
			return;
		}
	}
	if (q->n < EPD_QMAX) {
		*queue_at(q, q->n++) = in;
		return;
	}
	t = queue_at(q, q->n - 1);
	a = queue_rect(t);
	b = queue_rect(&in);
	box = queue_box(&a, &b);
	queue_set_rect(t, &box);
	t->upd.mode = in.upd.mode;
	t->upd.full |= in.upd.full;
	t->upd.repeat = in.upd.repeat;
	t->upd.marker = in.upd.marker;
	t->flags = in.flags;
	t->barrier |= in.barrier;
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
