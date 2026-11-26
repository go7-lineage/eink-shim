// SPDX-License-Identifier: GPL-2.0-only
/*
 * UI refresh policy.
 */
#include "epd_ui.h"

/* frames closer than this belong to one burst */
#define UI_BURST_MS		100
/* cooldown before a settle pass */
#define UI_SETTLE_MS		150
/* cooldown before a big burst's GC16 */
#define UI_BIG_QUIET_MS		400
/* a frame after this much quiet counts towards gc_interval */
#define UI_INTERACTION_MS	500
/* bursts longer than this never get GC16 */
#define UI_BIG_MAX_FRAMES	10
#define UI_FLASH_MIN_MS		500
#define UI_FLASH_QUIET_MS	150

#define UI_STOCK_TEMP		20

struct ui_mode {
	const char *name;
	int wf;
	u32 flag;
	bool debounced;
	bool inverted;
};

static const struct ui_mode ui_modes[EPD_UI_NMODES] = {
	[EPD_UI_SPEED] = { "speed", 4, 0x2000, false, false },
	[EPD_UI_NORMAL] = { "normal", 255, 0, true, false },
	[EPD_UI_REGAL] = { "regal", 5, 0x1000, true, false },
	[EPD_UI_REGALPLUS] = { "regalplus", 16, 0, true, false },
	[EPD_UI_NIGHT] = { "night", 15, 0x1000, true, true },
};

static const struct ui_mode ui_a2 = { "a2", 4, 0x2000, false, false };

#define UI_GC_WF	2
#define UI_GC_FLAG	0x1000

int epd_ui_mode_by_name(const char *name)
{
	int i;

	for (i = 0; i < EPD_UI_NMODES; i++)
		if (!strcmp(name, ui_modes[i].name))
			return i;
	return -EINVAL;
}

const char *epd_ui_mode_name(int mode)
{
	return mode >= 0 && mode < EPD_UI_NMODES ? ui_modes[mode].name : "?";
}

static const struct ui_mode *ui_mode(int mode)
{
	return &ui_modes[mode >= 0 && mode < EPD_UI_NMODES ? mode : EPD_UI_SPEED];
}

bool epd_ui_mode_inverted(int mode)
{
	return ui_mode(mode)->inverted;
}

int epd_ui_init(struct epd_ui *u, u32 w, u32 h)
{
	memset(u, 0, sizeof(*u));
	if (w < 2 || w % 2 || !h || w > 0xffff)
		return -EINVAL;
	u->w = w;
	u->h = h;
	u->shadow = epd_zalloc((size_t)w / 2 * h * sizeof(u64));
	u->rows = epd_zalloc(h * sizeof(*u->rows));
	u->burst = epd_zalloc(h * sizeof(*u->burst));
	u->transient = epd_zalloc(h * sizeof(*u->transient));
	if (!u->shadow || !u->rows || !u->burst || !u->transient) {
		epd_ui_release(u);
		return -ENOMEM;
	}
	u->settle_mode = -1;
	return 0;
}

void epd_ui_release(struct epd_ui *u)
{
	epd_free(u->shadow);
	epd_free(u->rows);
	epd_free(u->burst);
	epd_free(u->transient);
	u->shadow = NULL;
	u->rows = NULL;
	u->burst = NULL;
	u->transient = NULL;
}

void epd_ui_invalidate(struct epd_ui *u)
{
	u->valid = false;
}

enum ui_diff { UI_UNCHANGED, UI_CHANGED, UI_UNREADABLE };

static enum ui_diff ui_diff(struct epd_ui *u, const u8 *frame, u32 pitch, size_t len)
{
	u32 words = u->w / 2, y;
	size_t row_bytes = (size_t)words * 8;
	bool any = false;

	if (pitch < row_bytes || (size_t)pitch * u->h > len)
		return UI_UNREADABLE;
	for (y = 0; y < u->h; y++) {
		const u8 *src = frame + (size_t)y * pitch;
		u64 *shadow = u->shadow + (size_t)y * words;
		u32 a, z, i;

		if (u->valid) {
			u64 v;

			for (a = 0; a < words; a++) {
				memcpy(&v, src + (size_t)a * 8, 8);
				if (v != shadow[a])
					break;
			}
			if (a == words) {
				u->rows[y].end = 0;
				continue;
			}
			for (z = words - 1; z > a; z--) {
				memcpy(&v, src + (size_t)z * 8, 8);
				if (v != shadow[z])
					break;
			}
		} else {
			a = 0;
			z = words - 1;
		}
		for (i = a; i <= z; i++)
			memcpy(&shadow[i], src + (size_t)i * 8, 8);
		u->rows[y].start = (u16)(a * 2);
		u->rows[y].end = (u16)((z + 1) * 2);
		any = true;
	}
	u->valid = true;
	return any ? UI_CHANGED : UI_UNCHANGED;
}

static void ui_burst_reset(struct epd_ui *u)
{
	memset(u->burst, 0, u->h * sizeof(*u->burst));
	u->burst_frames = 0;
}

static void ui_spans_merge(u32 h, struct epd_ui_span *dst, const struct epd_ui_span *src)
{
	u32 y;

	for (y = 0; y < h; y++) {
		const struct epd_ui_span *f = &src[y];
		struct epd_ui_span *b = &dst[y];

		if (!f->end)
			continue;
		if (!b->end) {
			*b = *f;
		} else {
			b->start = b->start < f->start ? b->start : f->start;
			b->end = b->end > f->end ? b->end : f->end;
		}
	}
}

static void ui_burst_merge(struct epd_ui *u)
{
	ui_spans_merge(u->h, u->burst, u->rows);
}

static void ui_transient_reset(struct epd_ui *u)
{
	memset(u->transient, 0, u->h * sizeof(*u->transient));
	u->untracked = false;
}

struct ui_band {
	u32 y0, y1, x0, x1;
};

static u64 ui_band_area(const struct ui_band *b)
{
	return (u64)(b->x1 - b->x0) * (b->y1 - b->y0);
}

static struct ui_band ui_band_join(const struct ui_band *a, const struct ui_band *b)
{
	struct ui_band j = { a->y0, b->y1, a->x0 < b->x0 ? a->x0 : b->x0, a->x1 > b->x1 ? a->x1 : b->x1 };

	return j;
}

static u64 ui_band_join_cost(const struct ui_band *b, int i)
{
	struct ui_band j = ui_band_join(&b[i], &b[i + 1]);

	return ui_band_area(&j) - ui_band_area(&b[i]) - ui_band_area(&b[i + 1]);
}

static void ui_bands_squeeze(struct ui_band *b, int *n)
{
	u64 best_cost = ui_band_join_cost(b, 0), cost;
	int i, best = 0;

	for (i = 1; i + 1 < *n; i++) {
		cost = ui_band_join_cost(b, i);
		if (cost < best_cost) {
			best_cost = cost;
			best = i;
		}
	}
	b[best] = ui_band_join(&b[best], &b[best + 1]);
	for (i = best + 1; i + 1 < *n; i++)
		b[i] = b[i + 1];
	(*n)--;
}

static int ui_bands(const struct epd_ui *u, const struct epd_ui_span *a, const struct epd_ui_span *b,
		    struct ui_band *out)
{
	struct ui_band band[EPD_UI_MAX_RECTS + 1];
	bool open = false;
	int n = 0, i;
	u32 y;

	for (y = 0; y < u->h; y++) {
		u32 x0 = a[y].start, x1 = a[y].end;

		if (b && b[y].end) {
			x0 = !x1 || b[y].start < x0 ? b[y].start : x0;
			x1 = b[y].end > x1 ? b[y].end : x1;
		}
		if (!x1) {
			open = false;
			continue;
		}
		if (open) {
			struct ui_band row = { y, y + 1, x0, x1 };

			band[n - 1] = ui_band_join(&band[n - 1], &row);
			continue;
		}
		if (n == EPD_UI_MAX_RECTS + 1)
			ui_bands_squeeze(band, &n);
		band[n++] = (struct ui_band){ y, y + 1, x0, x1 };
		open = true;
	}
	if (n > EPD_UI_MAX_RECTS)
		ui_bands_squeeze(band, &n);
	for (i = 0; i < n; i++) {
		out[i] = band[i];
		out[i].x0 &= ~7u;
		out[i].x1 = (out[i].x1 + 7) & ~7u;
		if (out[i].x1 > u->w)
			out[i].x1 = u->w;
	}
	return n;
}

static bool ui_burst_add(struct epd_ui *u, bool new_burst, int pct)
{
	u64 area = 0;
	u32 y;

	if (new_burst)
		ui_burst_reset(u);
	ui_burst_merge(u);
	if (pct <= 0)
		return false;
	if (u->burst_frames < 0xffffffffu)
		u->burst_frames++;
	for (y = 0; y < u->h; y++)
		area += u->burst[y].end - u->burst[y].start;
	return u->burst_frames <= UI_BIG_MAX_FRAMES && area * 100 >= (u64)pct * u->w * u->h;
}

enum ui_kind { UI_GC16, UI_TRANSIENT, UI_NORMAL };

struct ui_decision {
	enum ui_kind kind;
	bool mode_changed;
	bool new_burst;
};

static void ui_decide(struct epd_ui *u, int mode, int gc_interval, u64 now, struct ui_decision *d)
{
	bool had = u->have_frame, gc = false;
	u64 since = had && now > u->last_frame ? now - u->last_frame : 0;
#define QUIET_FOR(ms) (!had || since >= (ms))

	u->last_frame = now;
	u->have_frame = true;
	d->mode_changed = !u->have_mode || u->last_mode != mode;
	if (d->mode_changed) {
		gc = u->have_mode;
		u->last_mode = mode;
		u->have_mode = true;
		u->settled = 0;
	}
	if (QUIET_FOR(UI_INTERACTION_MS) && gc_interval > 0) {
		if (u->settled < 0x7fffffff)
			u->settled++;
		gc |= u->settled > gc_interval;
	}
	if (gc) {
		u->settled = 0;
		d->kind = UI_GC16;
	} else if (ui_mode(mode)->debounced && !QUIET_FOR(UI_BURST_MS)) {
		d->kind = UI_TRANSIENT;
	} else {
		d->kind = UI_NORMAL;
	}
	d->new_burst = QUIET_FOR(UI_BIG_QUIET_MS);
#undef QUIET_FOR
}

static int ui_marker(struct epd_ui *u)
{
	u->marker = u->marker == 0x7fffffff ? 1 : u->marker + 1;
	return u->marker;
}

static void ui_upd(struct epd_request *r, u32 x, u32 y, u32 w, u32 h, int wf, u32 flag, bool full, int marker)
{
	r->top = (u16)y;
	r->left = (u16)x;
	r->width = (u16)w;
	r->height = (u16)h;
	r->mode = wf;
	r->update_mode = full;
	r->marker = (u32)marker;
	r->flags = flag;
}

void epd_ui_gc_request(struct epd_request *out, u32 w, u32 h)
{
	ui_upd(out, 0, 0, w, h, UI_GC_WF, UI_GC_FLAG, true, 0);
}

static void ui_flash_done(struct epd_ui *u, u64 now)
{
	u->flash_pending = false;
	u->have_flash = true;
	u->last_flash = now;
	if (u->flashes < 0xffffffffu)
		u->flashes++;
}

static void ui_full_shown(struct epd_ui *u, u64 now)
{
	u->settle_mode = -1;
	u->big_pending = false;
	if (u->flash_pending)
		ui_flash_done(u, now);
	ui_burst_reset(u);
	ui_transient_reset(u);
}

int epd_ui_seed(struct epd_ui *u, const u8 *rgba, u32 pitch, size_t len, u64 now)
{
	if (ui_diff(u, rgba, pitch, len) == UI_UNREADABLE) {
		u->valid = false;
		ui_full_shown(u, now);
		return -EINVAL;
	}
	u->settled = 0;
	ui_full_shown(u, now);
	return 0;
}

int epd_ui_flash(struct epd_ui *u, u64 now)
{
	if (u->flash_pending)
		return 0;
	if (u->have_flash && now < u->last_flash + UI_FLASH_MIN_MS) {
		if (u->flash_drops < 0xffffffffu)
			u->flash_drops++;
		return -EBUSY;
	}
	u->flash_pending = true;
	return 0;
}

static int ui_send(const struct epd_ui *u, const struct epd_ui_span *a, const struct epd_ui_span *b, int wf,
		   u32 flag, bool full, int marker, struct epd_request *out)
{
	struct ui_band band[EPD_UI_MAX_RECTS];
	int n = ui_bands(u, a, b, band), i;

	if (!n) {
		ui_upd(out, 0, 0, u->w, u->h, wf, flag, full, marker);
		return 1;
	}
	for (i = 0; i < n; i++)
		ui_upd(&out[i], band[i].x0, band[i].y0, band[i].x1 - band[i].x0, band[i].y1 - band[i].y0, wf, flag,
		       full, marker);
	return n;
}

int epd_ui_frame(struct epd_ui *u, const struct epd_ui_cfg *cfg, const u8 *rgba, u32 pitch, size_t len,
		 u64 now, struct epd_request *out, int *cut)
{
	const struct ui_mode *m = ui_mode(cfg->mode);
	struct ui_decision d;
	enum ui_diff diff = ui_diff(u, rgba, pitch, len);
	bool tracked = diff == UI_CHANGED;
	int marker;

	*cut = -1;
	if (diff == UI_UNCHANGED)
		return 0;
	ui_decide(u, cfg->mode, cfg->gc_interval, now, &d);
	if (d.mode_changed)
		*cut = m->debounced ? 0 : cfg->a2cut < 0 ? -1 : cfg->a2cut > 9 ? 9 : cfg->a2cut;
	marker = ui_marker(u);

	if (d.kind == UI_GC16) {
		ui_full_shown(u, now);
		if (tracked)
			ui_burst_merge(u);
		ui_upd(out, 0, 0, u->w, u->h, UI_GC_WF, UI_GC_FLAG, true, marker);
		return 1;
	}
	if (d.kind == UI_TRANSIENT) {
		u->settle_mode = cfg->mode;
		m = &ui_a2;
	}
	if (!tracked) {
		u->untracked = true;
		ui_upd(out, 0, 0, u->w, u->h, m->wf, m->flag, false, marker);
		return 1;
	}
	u->big_pending = ui_burst_add(u, d.new_burst, cfg->big_pct);
	if (d.kind == UI_TRANSIENT)
		ui_spans_merge(u->h, u->transient, u->rows);
	return ui_send(u, u->rows, NULL, m->wf, m->flag, false, marker, out);
}

enum epd_ui_step epd_ui_step(struct epd_ui *u, u64 now, struct epd_request *out, int *n, u64 *wait)
{
	const struct epd_ui_span *a, *b;
	bool flash = u->flash_pending;
	int wf;
	u32 flag;
	u64 quiet;

	*n = 0;
	if (flash) {
		wf = UI_GC_WF;
		flag = UI_GC_FLAG;
		quiet = UI_FLASH_QUIET_MS;
		a = NULL;
		b = NULL;
	} else if (u->big_pending) {
		wf = UI_GC_WF;
		flag = UI_GC_FLAG;
		quiet = UI_BIG_QUIET_MS;
		a = u->burst;
		b = u->transient;
	} else if (u->settle_mode >= 0) {
		wf = ui_mode(u->settle_mode)->wf;
		flag = ui_mode(u->settle_mode)->flag;
		quiet = UI_SETTLE_MS;
		a = u->transient;
		b = NULL;
	} else {
		return EPD_UI_IDLE;
	}
	if (u->have_frame && now < u->last_frame + quiet) {
		*wait = u->last_frame + quiet - now;
		return EPD_UI_WAIT;
	}
	if (flash) {
		ui_flash_done(u, now);
		u->settled = 0;
		ui_burst_reset(u);
	}
	u->settle_mode = -1;
	u->big_pending = false;
	if (flash || u->untracked) {
		ui_upd(out, 0, 0, u->w, u->h, wf, flag, true, ui_marker(u));
		*n = 1;
	} else {
		*n = ui_send(u, a, b, wf, flag, true, ui_marker(u), out);
	}
	ui_transient_reset(u);
	return EPD_UI_SEND;
}
