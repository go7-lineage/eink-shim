// SPDX-License-Identifier: GPL-2.0-only
/*
 * Update engine (stock: onyx_epdc_update_wb_sg, mergeWbBySg*Update_*).
 */
#include "epd_convert.h"
#include "epd_engine.h"
#include "epd_neon.h"
#include "epd_row.h"

struct epd_table {
	u8 *t;		/* n x 256, indexed by state */
	u16 n;
	int users;	/* active LUTs */
	bool retired;
	bool staged;
	u8 drive[256];
	u8 target[16];
};

struct epd_lut {
	bool active;
	u32 marker;
	struct epd_table *table;
	u16 frame;
	u16 y0, y1, x0, x1;
	u32 seq;
	int mode;
	bool full;
	bool repeat;
	bool quiet;
};

struct epd_req {
	struct epd_req *next;
	struct epd_update upd;
	u8 *target;		/* width x height, 4-bit target grey */
	u8 *mask;		/* 1 = still to merge */
	u16 y0, y1, x0, x1;	/* part of the rect that still has mask set */
	u64 wait[2];		/* LUTs this request waits for */
	bool wait_any;		/* ran out of LUTs, retry when any finish */
	u32 seq;
};

struct epd_engine {
	struct epd_engine_cfg cfg;
	struct epd_wbf wf;
	int t;
	u16 w, h;
	u8 *state;
	u8 *owner;
	u8 *mark;		/* Regal AA scratch */
	bool aa_all;
	bool gu_aa;
	struct epd_table **halo;
	struct epd_lut lut[EPD_NLUTS];
	u32 seq, req_seq;
	int active;
	struct epd_req *wait_head, **wait_tail;
	struct epd_table *mode14[2];	/* partial, full */
	struct epd_table **cache;	/* see cache_slot() */
	u8 zero[256];
	const u8 *cur[EPD_IDLE + 1];
	int finished[EPD_NLUTS];
	struct epd_lut done[EPD_NLUTS];
};

static bool lut_bit(const u64 *m, int i)
{
	return m[i >> 6] >> (i & 63) & 1;
}

#define CACHE_VARIANTS	8

static struct epd_table **cache_slot(struct epd_engine *e, int t, int slot, bool full, bool regal, bool aa)
{
	return &e->cache[(t * e->wf.n_modes + slot) * CACHE_VARIANTS + full * 4 + regal * 2 + aa];
}

static void table_free(struct epd_table *t)
{
	epd_free(t->t);
	epd_free(t);
}

static void cache_retire(struct epd_table **slot)
{
	struct epd_table *t = *slot;

	if (!t)
		return;
	*slot = NULL;
	if (t->users)
		t->retired = true;
	else
		table_free(t);
}

static void table_put(struct epd_table *t)
{
	if (--t->users == 0 && t->retired)
		table_free(t);
}

static u8 state_idle(u8 level)
{
	return (u8)(level << 4 | level);
}

static bool stage_level(u8 state)
{
	u8 level = state >> 4;

	return level == 0 || level == 15 || level != (state & 15);
}

static void table_index(struct epd_table *ct)
{
	bool ok[16] = { false }, any = false;
	int f, i, nw, old, lv, d;

	for (i = 0; i < 256; i++)
		for (f = 0; f < ct->n && !ct->drive[i]; f++)
			ct->drive[i] = ct->t[f * 256 + i] != 0;
	for (nw = 0; nw < 16; nw++)
		for (old = 0; old < 16; old++)
			if (old != nw && ct->drive[nw << 4 | old]) {
				ok[nw] = true;
				any = true;
			}
	for (lv = 0; lv < 16; lv++) {
		ct->target[lv] = (u8)lv;
		for (d = 0; any && d < 16; d++) {
			if (lv - d >= 0 && ok[lv - d]) {
				ct->target[lv] = (u8)(lv - d);
				break;
			}
			if (lv + d < 16 && ok[lv + d]) {
				ct->target[lv] = (u8)(lv + d);
				break;
			}
		}
	}
}

static int build_table(struct epd_engine *e, struct epd_table **slotp, int slot, bool full,
		       bool regal, bool aa)
{
	struct epd_table *ct;
	bool mode14 = slot < 0;
	int nfr = mode14 ? 30 : epd_wbf_frames(&e->wf, e->t, slot), n, f, nw, old, fix;
	u8 *src, *p4 = NULL;

	if (nfr < 1)
		return -EINVAL;
	ct = epd_zalloc(sizeof(*ct));
	if (!ct)
		return -ENOMEM;
	src = epd_zalloc((size_t)nfr * 1024);
	ct->t = epd_zalloc((size_t)nfr * 256);
	if (!src || !ct->t) {
		n = -ENOMEM;
		goto out;
	}
	if (mode14) {
		n = epd_mode14_frames(src, (size_t)nfr * 1024);
	} else {
		n = epd_wbf_decode(&e->wf, e->t, slot, src, (size_t)nfr * 1024);
		fix = epd_wbf_fixup_variant(&e->wf, slot);
		if (n > 0 && fix >= 0)
			epd_regal_fixup(src, n, fix);
	}
	if (n < 0)
		goto out;
	if (regal) {
		for (f = 0; f < n; f++)
			for (nw = 0; nw < 16; nw++)
				for (old = 0; old < 16; old++) {
					bool edge = aa && old == 15;
					int o5 = edge ? 31 : 2 * old;

					ct->t[f * 256 + (nw << 4 | old)] = full || nw != old || edge ?
						src[f * 1024 + 2 * nw * 32 + o5] : 0;
				}
	} else {
		p4 = epd_zalloc((size_t)nfr * 256);
		if (!p4) {
			n = -ENOMEM;
			goto out;
		}
		n = epd_tables4(src, nfr, e->wf.levels, slot, &e->cfg.tables, ct->t, p4);
		if (n > 0 && !full)
			memcpy(ct->t, p4, (size_t)n * 256);
		ct->staged = slot == e->cfg.tables.a2_slot && e->cfg.tables.a2_clean_mode == 2;
	}
	if (n > 0) {
		ct->n = (u16)n;
		table_index(ct);
	}
out:
	if (n < 0)
		table_free(ct);
	else
		*slotp = ct;
	epd_free(p4);
	epd_free(src);
	return n < 0 ? n : 0;
}

static int get_table(struct epd_engine *e, int mode, bool full, bool aa, struct epd_table **out)
{
	int slot = epd_mode_slot(e->wf.mode_version, mode), ret;
	bool regal = epd_mode_is_regal(mode);
	struct epd_table **ct;

	if (mode == 14) {
		ct = &e->mode14[full];
		slot = -1;
		regal = false;
		aa = false;
	} else {
		if (slot < 0 || slot >= e->wf.n_modes)
			return -EINVAL;
		aa &= regal;
		ct = cache_slot(e, e->t, slot, full, regal, aa);
	}
	if (!*ct) {
		ret = build_table(e, ct, slot, full, regal, aa);
		if (ret)
			return ret;
	}
	*out = *ct;
	return 0;
}

static int build_halo(struct epd_engine *e, struct epd_table **slotp, u16 n_gc16)
{
	int slot = epd_mode_slot(e->wf.mode_version, 5), nfr = 0, n = 0, fix, f, k;
	struct epd_table *ct;
	u8 *src = NULL;

	ct = epd_zalloc(sizeof(*ct));
	if (!ct)
		return -ENOMEM;
	if (slot >= 0 && slot < e->wf.n_modes)
		nfr = epd_wbf_frames(&e->wf, e->t, slot);
	if (nfr > 0) {
		src = epd_zalloc((size_t)nfr * 1024);
		if (!src)
			goto nomem;
		n = epd_wbf_decode(&e->wf, e->t, slot, src, (size_t)nfr * 1024);
		fix = epd_wbf_fixup_variant(&e->wf, slot);
		if (n > 0 && fix >= 0)
			epd_regal_fixup(src, n, fix);
	}
	k = n < n_gc16 ? n : n_gc16;
	k = k < 30 ? k : 30;
	if (k > 0) {
		ct->t = epd_zalloc((size_t)k * 256);
		if (!ct->t)
			goto nomem;
		for (f = 0; f < k; f++)
			ct->t[f * 256 + (15 << 4 | 15)] = src[(size_t)(n - k + f) * 1024 + 30 * 32 + 31];
		ct->n = (u16)k;
		table_index(ct);
	}
	epd_free(src);
	*slotp = ct;
	return 0;
nomem:
	epd_free(src);
	table_free(ct);
	return -ENOMEM;
}

static struct epd_table *get_halo(struct epd_engine *e, const struct epd_update *u, const struct epd_table *table)
{
	struct epd_table **ct = &e->halo[e->t];

	if (!e->gu_aa || u->full || epd_mode_is_regal(u->mode) ||
	    epd_mode_slot(e->wf.mode_version, u->mode) != epd_mode_slot(e->wf.mode_version, EPD_MODE_GC16))
		return NULL;
	if (!*ct && build_halo(e, ct, table->n))
		return NULL;
	return (*ct)->drive[15 << 4 | 15] ? *ct : NULL;
}

static void req_free(struct epd_req *r)
{
	epd_free(r->target);
	epd_free(r->mask);
	epd_free(r);
}

static void marker_complete(struct epd_engine *e, u32 marker)
{
	struct epd_req *r;
	int i;

	for (i = 0; i < EPD_NLUTS; i++)
		if (e->lut[i].active && e->lut[i].marker == marker)
			return;
	for (r = e->wait_head; r; r = r->next)
		if (r->upd.marker == marker)
			return;
	if (e->cfg.marker_done)
		e->cfg.marker_done(e->cfg.ctx, marker);
}

static void wait_append(struct epd_engine *e, struct epd_req *r)
{
	struct epd_req **pp = &e->wait_head;

	while (*pp && (*pp)->seq < r->seq)
		pp = &(*pp)->next;
	r->next = *pp;
	*pp = r;
	if (!r->next)
		e->wait_tail = &r->next;
}

static bool older_overlaps(const struct epd_engine *e, const struct epd_req *r)
{
	const struct epd_update *u = &r->upd;
	const struct epd_req *w;

	for (w = e->wait_head; w && w->seq < r->seq; w = w->next) {
		const struct epd_update *o = &w->upd;

		if ((u32)o->left + w->x0 < (u32)u->left + r->x1 && (u32)u->left + r->x0 < (u32)o->left + w->x1 &&
		    (u32)o->top + w->y0 < (u32)u->top + r->y1 && (u32)u->top + r->y0 < (u32)o->top + w->y1)
			return true;
	}
	return false;
}

static bool older_wants(const struct epd_engine *e, const struct epd_req *r, u32 ax, u32 ay)
{
	const struct epd_req *w;

	for (w = e->wait_head; w && w->seq < r->seq; w = w->next) {
		const struct epd_update *o = &w->upd;

		if (ax < (u32)o->left + w->x0 || ax >= (u32)o->left + w->x1 ||
		    ay < (u32)o->top + w->y0 || ay >= (u32)o->top + w->y1)
			continue;
		if (w->mask[(size_t)(ay - o->top) * o->width + (ax - o->left)])
			return true;
	}
	return false;
}

struct claim {
	int lut;
	int n;
	u16 y0, y1, x0, x1;
};

static int free_lut(const struct epd_engine *e, int skip)
{
	int i;

	for (i = 0; i < EPD_NLUTS; i++)
		if (!e->lut[i].active && i != skip)
			return i;
	return -1;
}

static void claim_add(struct claim *c, u32 x, u32 y)
{
	c->n++;
	c->y0 = (u16)(y < c->y0 ? y : c->y0);
	c->y1 = (u16)(y + 1 > c->y1 ? y + 1 : c->y1);
	c->x0 = (u16)(x < c->x0 ? x : c->x0);
	c->x1 = (u16)(x + 1 > c->x1 ? x + 1 : c->x1);
}

static struct epd_lut *activate(struct epd_engine *e, const struct claim *c, struct epd_table *table,
				u32 left, u32 top, u32 marker)
{
	struct epd_lut *l = &e->lut[c->lut];

	memset(l, 0, sizeof(*l));
	l->active = true;
	l->marker = marker;
	l->table = table;
	table->users++;
	l->y0 = (u16)(top + c->y0);
	l->y1 = (u16)(top + c->y1);
	l->x0 = (u16)(left + c->x0);
	l->x1 = (u16)(left + c->x1);
	l->seq = e->seq++;
	e->active++;
	return l;
}

static void activate_update(struct epd_engine *e, const struct claim *c, struct epd_table *table,
			    const struct epd_update *u, int mode, bool repeat)
{
	struct epd_lut *l = activate(e, c, table, u->left, u->top, u->marker);

	l->mode = mode;
	l->full = mode == u->mode ? u->full : true;
	l->repeat = repeat && table->staged;
}

#define MASK_TODO	1
#define MASK_EDGE	2
#define AA_DARK		0
#define AA_WHITE	15
#define AA_OTHER	16

static void regal_aa_mark(struct epd_engine *e, struct epd_req *r)
{
	const struct epd_update *u = &r->upd;
	size_t n = (size_t)u->width * u->height, i;
	u32 x, y;

	for (y = 0; y < u->height; y++) {
		const u8 *state = e->state + (size_t)(u->top + y) * e->w + u->left;
		const u8 *target = r->target + (size_t)y * u->width;
		u8 *kind = e->mark + (size_t)y * u->width;

		for (x = 0; x < u->width; x++) {
			u8 level = state[x] >> 4;

			kind[x] = level == 15 ? AA_WHITE : e->aa_all || target[x] != level ? AA_DARK : AA_OTHER;
		}
	}
#ifdef EPD_NEON
	if (u->width >= 2 && u->height >= 2) {
		EPD_SIMD_STATE;

		epd_simd_begin();
		epd_regal_aa_mark_neon(e->mark, u->width, u->width, u->height, r->mask, 0);
		epd_simd_end();
	} else {
		epd_regal_aa_mark(e->mark, u->width, u->width, u->height, r->mask, 0);
	}
#else
	epd_regal_aa_mark(e->mark, u->width, u->width, u->height, r->mask, 0);
#endif
	for (i = 0; i < n; i++)
		r->mask[i] = (u8)(MASK_TODO | (r->mask[i] ? MASK_EDGE : 0));
}

static void halo_claim(struct epd_engine *e, const struct epd_req *r, const struct claim *c, struct claim *h)
{
	static const int dx[4] = { -1, 1, 0, 0 }, dy[4] = { 0, 0, -1, 1 };
	const struct epd_update *u = &r->upd;
	u32 x, y;
	int k;

	for (y = c->y0; y < c->y1; y++)
		for (x = c->x0; x < c->x1; x++) {
			u32 px = u->left + x, py = u->top + y;
			size_t p = (size_t)py * e->w + px;

			if (e->owner[p] != c->lut || e->state[p] >> 4 != 15 || (e->state[p] & 15) == 15)
				continue;
			for (k = 0; k < 4; k++) {
				u32 qx = px + (u32)dx[k], qy = py + (u32)dy[k];
				size_t q = (size_t)qy * e->w + qx;

				if (qx >= e->w || qy >= e->h || e->owner[q] != EPD_IDLE || e->state[q] != 0xff)
					continue;
				e->owner[q] = (u8)h->lut;
				claim_add(h, qx, qy);
			}
		}
}

static int merge(struct epd_engine *e, struct epd_req *r)
{
	const struct epd_update *u = &r->upd;
	struct epd_table *table, *aat = NULL, *fallback = NULL, *halo;
	struct claim c = { .y0 = 0xffff, .x0 = 0xffff }, ca = { .lut = -1, .y0 = 0xffff, .x0 = 0xffff };
	struct claim busy = { .lut = -1, .y0 = 0xffff, .x0 = 0xffff };
	struct claim h = { .lut = -1, .y0 = 0xffff, .x0 = 0xffff };
	bool aa = e->cfg.regal_aa && epd_mode_has_aa(u->mode), older = older_overlaps(e, r);
	int ret;
	u32 x, y;

	ret = get_table(e, u->mode, u->full, false, &table);
	if (!ret && aa)
		ret = get_table(e, u->mode, u->full, true, &aat);
	if (ret) {
		req_free(r);
		return ret;
	}
	if (u->mode == EPD_MODE_A2 && get_table(e, EPD_MODE_DU, true, false, &fallback))
		fallback = NULL;
	halo = get_halo(e, u, table);
	c.lut = free_lut(e, -1);
	r->wait[0] = 0;
	r->wait[1] = 0;
	r->wait_any = false;

	for (y = r->y0; y < r->y1; y++) {
		size_t row = (size_t)(u->top + y) * e->w + u->left;
		size_t rrow = (size_t)y * u->width;

		for (x = r->x0; x < r->x1; x++) {
			size_t p = row + x;
			bool edge = aa && (r->mask[rrow + x] & MASK_EDGE);
			struct epd_table *tab = edge ? aat : table;
			struct claim *cl = edge ? &ca : &c;
			u8 o, tgt, cur, idx;

			if (!r->mask[rrow + x])
				continue;
			o = e->owner[p];
			if (o != EPD_IDLE) {
				r->wait[o >> 6] |= 1ull << (o & 63);
				claim_add(&busy, x, y);
				continue;
			}
			if (older && older_wants(e, r, u->left + x, u->top + y)) {
				r->wait_any = true;
				claim_add(&busy, x, y);
				continue;
			}
			tgt = r->target[rrow + x];
			cur = e->state[p] >> 4;
			idx = (u8)(tgt << 4 | cur);
			if (fallback && fallback->drive[idx] &&
			    (!tab->drive[idx] || (tab->staged && !stage_level(e->state[p])))) {
				tab = fallback;
				cl = &ca;
			}
			if (!tab->drive[idx] || (!u->full && !edge && cur == tgt)) {
				e->state[p] = state_idle(cur);
				r->mask[rrow + x] = 0;
				continue;
			}
			if (cl == &ca && ca.lut < 0)
				ca.lut = free_lut(e, c.lut);
			if (cl->lut < 0) {
				r->wait_any = true;
				claim_add(&busy, x, y);
				continue;
			}
			e->state[p] = idx;
			e->owner[p] = (u8)cl->lut;
			r->mask[rrow + x] = 0;
			claim_add(cl, x, y);
		}
	}

	if (halo && c.n && ca.lut < 0) {
		h.lut = free_lut(e, c.lut);
		if (h.lut >= 0)
			halo_claim(e, r, &c, &h);
	}
	if (c.n)
		activate_update(e, &c, table, u, u->mode, u->repeat);
	if (ca.n)
		activate_update(e, &ca, aa ? aat : fallback, u, aa ? u->mode : EPD_MODE_DU, false);
	if (h.n)
		activate(e, &h, halo, 0, 0, u->marker)->mode = u->mode;
	if (busy.n) {
		r->y0 = busy.y0;
		r->y1 = busy.y1;
		r->x0 = busy.x0;
		r->x1 = busy.x1;
		wait_append(e, r);
	} else {
		u32 marker = u->marker;

		req_free(r);
		if (!c.n && !ca.n)
			marker_complete(e, marker);
	}
	return 0;
}

static void supersede(struct epd_engine *e, const struct epd_update *u)
{
	u32 ux1 = (u32)u->left + u->width, uy1 = (u32)u->top + u->height;
	struct epd_req **pp, *w;

	for (pp = &e->wait_head; (w = *pp);) {
		const struct epd_update *o = &w->upd;
		u32 wx0 = (u32)o->left + w->x0, wx1 = (u32)o->left + w->x1;
		u32 wy0 = (u32)o->top + w->y0, wy1 = (u32)o->top + w->y1;
		u32 x0 = wx0 > u->left ? wx0 : u->left, x1 = wx1 < ux1 ? wx1 : ux1;
		u32 y0 = wy0 > u->top ? wy0 : u->top, y1 = wy1 < uy1 ? wy1 : uy1;
		u32 y;

		if (o->mode != u->mode || o->full != u->full || x0 >= x1 || y0 >= y1) {
			pp = &w->next;
			continue;
		}
		if (x0 == wx0 && x1 == wx1 && y0 == wy0 && y1 == wy1) {
			u32 marker = o->marker;

			*pp = w->next;
			if (!*pp)
				e->wait_tail = pp;
			req_free(w);
			marker_complete(e, marker);
			continue;
		}
		for (y = y0; y < y1; y++)
			memset(w->mask + (size_t)(y - o->top) * o->width + (x0 - o->left), 0, x1 - x0);
		pp = &w->next;
	}
}

int epd_engine_submit(struct epd_engine *e, const struct epd_update *u, const u8 *grey8,
		      size_t stride)
{
	struct epd_table *table;
	struct epd_req *r;
	size_t n;
	u32 x, y;
	int ret;

	if ((u32)u->top + u->height > e->h || (u32)u->left + u->width > e->w)
		return -EINVAL;
	ret = get_table(e, u->mode, u->full, false, &table);
	if (ret)
		return ret;
	n = (size_t)u->width * u->height;
	r = epd_zalloc(sizeof(*r));
	if (!r)
		return -ENOMEM;
	r->upd = *u;
	r->target = epd_zalloc(n);
	r->mask = epd_zalloc(n);
	if (!r->target || !r->mask) {
		req_free(r);
		return -ENOMEM;
	}
	for (y = 0; y < u->height; y++) {
		const u8 *src = grey8 + (size_t)(u->top + y) * stride + u->left;

		for (x = 0; x < u->width; x++)
			r->target[(size_t)y * u->width + x] = table->target[src[x] >> 4];
	}
	if (e->cfg.regal_aa && epd_mode_has_aa(u->mode))
		regal_aa_mark(e, r);
	else
		memset(r->mask, MASK_TODO, n);
	r->y1 = u->height;
	r->x1 = u->width;
	r->seq = e->req_seq++;
	if (e->cfg.supersede)
		supersede(e, u);
	return merge(e, r);
}

bool epd_engine_busy(const struct epd_engine *e)
{
	return e->active || e->wait_head;
}

static void release_owner(struct epd_engine *e, const struct epd_lut *l, u8 id)
{
	u32 y, x, n = l->x1 - l->x0;

	for (y = l->y0; y < l->y1; y++) {
		u8 *o = e->owner + (size_t)y * e->w + l->x0;
		u8 *s = e->state + (size_t)y * e->w + l->x0;

		if (!l->table->staged) {
			for (x = 0; x < n; x++) {
				bool mine = o[x] == id;

				s[x] = mine ? (u8)((s[x] & 0xf0) | s[x] >> 4) : s[x];
				o[x] = mine ? EPD_IDLE : o[x];
			}
			continue;
		}
		for (x = 0; x < n; x++) {
			u8 tgt = s[x] >> 4, level;

			if (o[x] != id)
				continue;
			level = epd_a2_level(tgt, s[x] & 15);
			s[x] = (u8)(level << 4 | tgt);
			o[x] = EPD_IDLE;
		}
	}
}

static void touch_up(struct epd_engine *e, const struct epd_table *table, struct claim *c, u32 x, u32 y,
		     bool white)
{
	u8 at = white ? 0xff : 0x00, idx = white ? 0xf8 : 0x07;
	u32 qx, qy;

	if (!table->drive[idx])
		return;
	for (qy = y ? y - 1 : y; qy <= y + 1 && qy < e->h; qy++)
		for (qx = x ? x - 1 : x; qx <= x + 1 && qx < e->w; qx++) {
			size_t q = (size_t)qy * e->w + qx;

			if (e->owner[q] != EPD_IDLE || e->state[q] != at)
				continue;
			e->state[q] = idx;
			e->owner[q] = (u8)c->lut;
			claim_add(c, qx, qy);
		}
}

static void repeat(struct epd_engine *e, const struct epd_lut *l)
{
	struct claim c = { .lut = free_lut(e, -1), .y0 = 0xffff, .x0 = 0xffff };
	struct epd_table *table = NULL;
	struct epd_lut *next;
	u32 x, y;

	if (c.lut >= 0 && get_table(e, l->mode, l->full, false, &table))
		c.lut = -1;
	for (y = l->y0; y < l->y1; y++) {
		size_t row = (size_t)y * e->w;

		for (x = l->x0; x < l->x1; x++) {
			u8 s = e->state[row + x], idx = (u8)(s << 4 | s >> 4);

			if (e->owner[row + x] != EPD_IDLE || s >> 4 == (s & 15))
				continue;
			if (c.lut < 0 || !table->drive[idx]) {
				e->state[row + x] = state_idle(s >> 4);
				continue;
			}
			e->state[row + x] = idx;
			e->owner[row + x] = (u8)c.lut;
			claim_add(&c, x, y);
			if (s == 0xef || s == 0x10)
				touch_up(e, table, &c, x, y, s == 0xef);
		}
	}
	if (!c.n)
		return;
	next = activate(e, &c, table, 0, 0, l->marker);
	next->mode = l->mode;
	next->full = l->full;
	next->repeat = true;
	next->quiet = true;
}

static void clear_words(u32 *row, u32 from, u32 to)
{
	for (; from < to; from++)
		row[from] &= ~0xffffu;
}

void epd_row_scalar(const u8 *state, const u8 *owner, const u8 *const *cur, u32 *out, u32 npix)
{
	epd_row_pack(state, owner, cur, out, npix);
}

#ifdef EPD_NEON
#define epd_row epd_row_neon
#else
#define epd_row epd_row_scalar
#endif

int epd_engine_tick(struct epd_engine *e, u32 *buf)
{
	EPD_SIMD_STATE;
	const struct epd_timing *t = e->cfg.timing;
	u32 lt = epd_lt(t), start = epd_data_start(t), y;
	const u8 **cur = e->cur;
	int *finished = e->finished;
	u32 y0 = 0xffff, y1 = 0, x0 = 0xffff, x1 = 0;
	int nfin = 0, i, j;
	struct epd_req *ready = NULL, **ready_tail = &ready, **pp, *r;

	if (!epd_engine_busy(e))
		return 0;

	for (i = 0; i <= EPD_IDLE; i++)
		cur[i] = e->zero;
	for (i = 0; i < EPD_NLUTS; i++) {
		struct epd_lut *l = &e->lut[i];

		if (!l->active)
			continue;
		cur[i] = l->table->t + (size_t)l->frame * 256;
		y0 = l->y0 < y0 ? l->y0 : y0;
		y1 = l->y1 > y1 ? l->y1 : y1;
		x0 = l->x0 < x0 ? l->x0 : x0;
		x1 = l->x1 > x1 ? l->x1 : x1;
	}
	x0 &= ~15u;
	x1 = (x1 + 15) & ~15u;
	if (x1 > e->w)
		x1 = e->w;
	epd_simd_begin();
	for (y = 0; y < e->h; y++) {
		u32 *row = buf + start + (size_t)y * lt;
		size_t p = (size_t)y * e->w;

		if (y < y0 || y >= y1) {
			clear_words(row, 0, e->w / 8u);
			continue;
		}
		clear_words(row, 0, x0 / 8);
		epd_row(e->state + p + x0, e->owner + p + x0, cur, row + x0 / 8, x1 - x0);
		clear_words(row, x1 / 8, e->w / 8u);
		if ((y & 63) == 63) {
			epd_simd_end();
			epd_simd_begin();
		}
	}
	epd_simd_end();

	for (i = 0; i < EPD_NLUTS; i++) {
		struct epd_lut *l = &e->lut[i];

		if (l->active && ++l->frame == l->table->n)
			finished[nfin++] = i;
	}
	for (i = 1; i < nfin; i++)
		for (j = i; j > 0 && e->lut[finished[j]].seq < e->lut[finished[j - 1]].seq; j--) {
			int tmp = finished[j];

			finished[j] = finished[j - 1];
			finished[j - 1] = tmp;
		}
	for (i = 0; i < nfin; i++) {
		struct epd_lut *l = &e->lut[finished[i]];

		release_owner(e, l, (u8)finished[i]);
		table_put(l->table);
		l->active = false;
		e->active--;
		e->done[i] = *l;
		if (!l->quiet)
			marker_complete(e, l->marker);
	}
	if (!nfin)
		return 1;

	for (pp = &e->wait_head; (r = *pp);) {
		bool wait = false;

		for (i = 0; i < EPD_NLUTS && !wait; i++)
			wait = lut_bit(r->wait, i) && e->lut[i].active;
		if (wait) {
			pp = &r->next;
			continue;
		}
		*pp = r->next;
		r->next = NULL;
		*ready_tail = r;
		ready_tail = &r->next;
	}
	e->wait_tail = pp;
	while (ready) {
		u32 marker;

		r = ready;
		ready = r->next;
		marker = r->upd.marker;
		if (merge(e, r))
			marker_complete(e, marker);
	}
	for (i = 0; i < nfin; i++)
		if (e->done[i].repeat)
			repeat(e, &e->done[i]);
	return 1;
}

int epd_engine_mode_frames(struct epd_engine *e, int mode, bool full)
{
	struct epd_table *table;
	int ret = get_table(e, mode, full, false, &table);

	return ret ? ret : table->n;
}

int epd_engine_frame_rate(const struct epd_engine *e)
{
	return e->wf.frame_rate;
}

int epd_engine_level(const struct epd_engine *e, u32 x, u32 y)
{
	return x < e->w && y < e->h ? e->state[(size_t)y * e->w + x] >> 4 : -EINVAL;
}

int epd_engine_set_temp_mc(struct epd_engine *e, int temp_mc)
{
	int t = epd_temp_index_hyst(&e->wf, e->t, temp_mc);
	size_t per_t = (size_t)e->wf.n_modes * CACHE_VARIANTS, i;

	if (t != e->t) {
		for (i = 0; i < (size_t)e->wf.n_temps * per_t; i++)
			if (i / per_t == (size_t)e->t)
				cache_retire(&e->cache[i]);
		cache_retire(&e->halo[e->t]);
		e->t = t;
	}
	return 0;
}

void epd_engine_temp_range(const struct epd_engine *e, int *lo, int *hi)
{
	*lo = e->wf.temp_bounds[e->t];
	*hi = e->wf.temp_bounds[e->t + 1];
}

int epd_engine_set_cut(struct epd_engine *e, int cut)
{
	int slots[2] = { e->cfg.tables.a2_slot, e->cfg.tables.du_slot }, t, k, v;

	if (cut < 0 || cut > 255)
		return -EINVAL;
	e->cfg.tables.cut = (u8)cut;
	for (t = 0; t < e->wf.n_temps; t++)
		for (k = 0; k < 2; k++)
			if (slots[k] >= 0 && slots[k] < e->wf.n_modes)
				for (v = 0; v < CACHE_VARIANTS; v++)
					cache_retire(&e->cache[(t * e->wf.n_modes + slots[k]) * CACHE_VARIANTS + v]);
	return 0;
}

void epd_engine_set_aa_all(struct epd_engine *e, bool all)
{
	e->aa_all = all;
}

void epd_engine_set_gu_aa(struct epd_engine *e, bool on)
{
	e->gu_aa = on;
}

int epd_engine_get_cut(const struct epd_engine *e)
{
	return e->cfg.tables.cut;
}

struct epd_engine *epd_engine_create(const struct epd_engine_cfg *cfg, const u8 *wbf, size_t size,
				     int *err)
{
	struct epd_engine *e;
	size_t px;
	int ret;

	e = epd_zalloc(sizeof(*e));
	if (!e) {
		*err = -ENOMEM;
		return NULL;
	}
	e->cfg = *cfg;
	e->wait_tail = &e->wait_head;
	ret = epd_wbf_parse(&e->wf, wbf, size);
	if (ret)
		goto err_free;
	ret = -EINVAL;
	if (e->wf.levels != 32 || cfg->timing->width % 8)
		goto err_wbf;
	e->cfg.tables.a2_slot = epd_mode_slot(e->wf.mode_version, 4);
	e->cfg.tables.du_slot = epd_mode_slot(e->wf.mode_version, 1);
	e->t = epd_temp_index(&e->wf, cfg->temp_c);
	e->w = cfg->timing->width;
	e->h = cfg->timing->height;
	px = (size_t)e->w * e->h;
	ret = -ENOMEM;
	e->state = epd_zalloc(px);
	e->owner = epd_zalloc(px);
	e->cache = epd_zalloc(sizeof(*e->cache) * e->wf.n_temps * e->wf.n_modes * CACHE_VARIANTS);
	e->halo = epd_zalloc(sizeof(*e->halo) * e->wf.n_temps);
	e->mark = epd_zalloc(px);
	if (!e->state || !e->owner || !e->cache || !e->halo || !e->mark)
		goto err_mem;
	memset(e->owner, EPD_IDLE, px);
	return e;

err_mem:
	epd_free(e->state);
	epd_free(e->owner);
	epd_free(e->mark);
	epd_free(e->cache);
	epd_free(e->halo);
err_wbf:
	epd_wbf_release(&e->wf);
err_free:
	epd_free(e);
	*err = ret;
	return NULL;
}

void epd_engine_destroy(struct epd_engine *e)
{
	struct epd_req *r;
	size_t k, n;
	int i;

	if (!e)
		return;
	while ((r = e->wait_head)) {
		e->wait_head = r->next;
		req_free(r);
	}
	for (i = 0; i < EPD_NLUTS; i++)
		if (e->lut[i].active)
			table_put(e->lut[i].table);
	n = (size_t)e->wf.n_temps * e->wf.n_modes * CACHE_VARIANTS;
	for (k = 0; k < n; k++)
		cache_retire(&e->cache[k]);
	for (k = 0; k < e->wf.n_temps; k++)
		cache_retire(&e->halo[k]);
	cache_retire(&e->mode14[0]);
	cache_retire(&e->mode14[1]);
	epd_free(e->cache);
	epd_free(e->halo);
	epd_free(e->state);
	epd_free(e->owner);
	epd_free(e->mark);
	epd_wbf_release(&e->wf);
	epd_free(e);
}
