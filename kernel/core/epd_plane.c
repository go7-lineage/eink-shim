// SPDX-License-Identifier: GPL-2.0-only
#include "epd_convert.h"
#include "epd_plane.h"

int epd_plane_init(struct epd_plane *pl, u32 w, u32 h, bool du_a2_clean)
{
	size_t n = (size_t)w * h;

	memset(pl, 0, sizeof(*pl));
	if (!w || !h)
		return -EINVAL;
	pl->w = w;
	pl->h = h;
	pl->grey = epd_zalloc(n);
	pl->in = epd_zalloc(n);
	pl->out = epd_zalloc(n);
	pl->err = epd_zalloc((3 * (size_t)w + 12) * sizeof(*pl->err));
	if (!pl->grey || !pl->in || !pl->out || !pl->err) {
		epd_plane_release(pl);
		return -ENOMEM;
	}
	memset(pl->grey, 0xff, n);
	memset(pl->in, 0xff, n);
	epd_policy_init(&pl->pol, du_a2_clean);
	epd_queue_init(&pl->q);
	pl->q.merge = true;
	return 0;
}

void epd_plane_release(struct epd_plane *pl)
{
	epd_free(pl->grey);
	epd_free(pl->in);
	epd_free(pl->out);
	epd_free(pl->err);
	pl->grey = NULL;
	pl->in = NULL;
	pl->out = NULL;
	pl->err = NULL;
}

static void plane_copy(const struct epd_plane *pl, u8 *dst, const u8 *src, u32 left, u32 top, u32 w, u32 h)
{
	u32 y;

	for (y = top; y < top + h; y++)
		memcpy(dst + (size_t)y * pl->w + left, src + (size_t)y * pl->w + left, w);
}

void epd_plane_push(struct epd_plane *pl, const struct epd_request *r, int n)
{
	struct epd_decision first, d;
	int i;

	for (i = 0; i < n; i++) {
		if (i) {
			epd_policy_apply_next(&first, &r[i], &d);
		} else {
			epd_policy_apply(&pl->pol, &r[0], &first);
			d = first;
		}
		plane_copy(pl, pl->grey, pl->in, r[i].left, r[i].top, r[i].width, r[i].height);
		epd_queue_push(&pl->q, &d);
	}
}

bool epd_plane_pop(struct epd_plane *pl, struct epd_engine *e, u32 conv, struct epd_decision *d, int *ret)
{
	const struct epd_update *u = &d->upd;
	const u8 *src = pl->grey;
	u32 flags, ml = 0, mt = 0;

	if (!epd_queue_pop(&pl->q, epd_engine_busy(e), d))
		return false;
	flags = d->flags | conv;
	if (flags & EPD_DITHER_FLAGS) {
		if ((flags & (EPD_CONV_Y1_ORDERED | 0x2000)) != (EPD_CONV_Y1_ORDERED | 0x2000)) {
			ml = u->left < EPD_PLANE_WARM ? u->left : EPD_PLANE_WARM;
			mt = u->top < EPD_PLANE_WARM ? u->top : EPD_PLANE_WARM;
		}
		plane_copy(pl, pl->out, pl->grey, u->left - ml, u->top - mt, u->width + ml, u->height + mt);
		epd_quantize_update(pl->out, pl->w, u->left - ml, u->top - mt, u->width + ml, u->height + mt, flags,
				    pl->err);
		src = pl->out;
	}
	*ret = epd_engine_submit(e, u, src, pl->w);
	return true;
}
