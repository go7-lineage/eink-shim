// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>
#include <string.h>

#include "wbf_build.h"

struct out {
	u8 *d;
	size_t n, cap;
};

static void put(struct out *o, u8 b)
{
	if (o->n == o->cap) {
		u8 *d;

		o->cap = o->cap ? 2 * o->cap : 4096;
		d = realloc(o->d, o->cap);
		if (!d)
			abort();
		o->d = d;
	}
	o->d[o->n++] = b;
}

static u8 sum8(const u8 *d, size_t from, size_t to)
{
	u8 s = 0;

	while (from < to)
		s = (u8)(s + d[from++]);
	return s;
}

void wbf_set_ptr(u8 *d, size_t at, size_t target)
{
	d[at] = (u8)target;
	d[at + 1] = (u8)(target >> 8);
	d[at + 2] = (u8)(target >> 16);
	d[at + 3] = (u8)(d[at] + d[at + 1] + d[at + 2]);
}

void wbf_fix_header(u8 *d)
{
	d[31] = sum8(d, 8, 31);
	d[47] = sum8(d, 32, 47);
}

static size_t get_ptr(const u8 *d, size_t at)
{
	return (size_t)d[at] | (size_t)d[at + 1] << 8 | (size_t)d[at + 2] << 16;
}

size_t wbf_temp_table(const struct wbf_blob *b, unsigned int mode)
{
	return get_ptr(b->data, b->mode_table + 4 * (size_t)mode);
}

size_t wbf_block(const struct wbf_blob *b, unsigned int mode, unsigned int temp)
{
	return get_ptr(b->data, wbf_temp_table(b, mode) + 4 * (size_t)temp);
}

static u8 packed(const struct wbf_spec *s, unsigned int mode, unsigned int temp, unsigned int levels, size_t i)
{
	u8 b = 0;
	unsigned int k;

	for (k = 0; k < 4; k++) {
		size_t e = 4 * i + k, l2 = (size_t)levels * levels;
		unsigned int f = (unsigned int)(e / l2), nw = (unsigned int)(e % l2 / levels);
		unsigned int old = (unsigned int)(e % levels);
		u8 v = s->drive(mode, temp, f, nw, old);

		if (v > 2)
			abort();
		b = (u8)(b | v << (2 * k));
	}
	return b;
}

static void put_block(struct out *o, const struct wbf_spec *s, unsigned int mode, unsigned int temp,
		      unsigned int levels)
{
	size_t n = (size_t)s->frames(mode, temp) * levels * levels / 4, i = 0;
	bool literal = false;
	u8 total = 0;

	while (i < n) {
		u8 b = packed(s, mode, temp, levels, i);
		size_t run = 1;

		while (i + run < n && run < 256 && packed(s, mode, temp, levels, i + run) == b)
			run++;
		if ((run == 1) != literal) {
			put(o, WBF_TOGGLE);
			total = (u8)(total + WBF_TOGGLE);
			literal = !literal;
		}
		if (literal) {
			put(o, b);
			total = (u8)(total + b);
		} else {
			put(o, b);
			put(o, (u8)(run - 1));
			total = (u8)(total + b + (u8)(run - 1));
		}
		i += run;
	}
	put(o, WBF_END);
	put(o, (u8)(total + WBF_END));
}

void wbf_build(const struct wbf_spec *s, struct wbf_blob *out)
{
	unsigned int levels = s->lut_fmt == 4 || s->mode_version == 0x90 ? 32 : 16, m, t;
	size_t name_len = strlen(s->name), i, mode_table;
	struct out o = { NULL, 0, 0 };

	for (i = 0; i < 0x30; i++)
		put(&o, 0);
	for (i = 0; i <= s->n_temps; i++)
		put(&o, s->temp_bounds[i]);
	put(&o, sum8(o.d, 0x30, o.n));
	put(&o, (u8)name_len);
	for (i = 0; i < name_len; i++)
		put(&o, (u8)s->name[i]);
	mode_table = o.n;
	for (i = 0; i < 4 * (size_t)s->n_modes * (1 + s->n_temps); i++)
		put(&o, 0);
	for (m = 0; m < s->n_modes; m++) {
		size_t temp_table = mode_table + 4 * (size_t)s->n_modes + 4 * (size_t)m * s->n_temps;

		wbf_set_ptr(o.d, mode_table + 4 * (size_t)m, temp_table);
		for (t = 0; t < s->n_temps; t++) {
			wbf_set_ptr(o.d, temp_table + 4 * (size_t)t, o.n);
			put_block(&o, s, m, t, levels);
		}
	}
	o.d[0x10] = s->mode_version;
	o.d[0x18] = s->frame_rate;
	o.d[0x20] = (u8)mode_table;
	o.d[0x21] = (u8)(mode_table >> 8);
	o.d[0x22] = (u8)(mode_table >> 16);
	o.d[0x24] = s->lut_fmt;
	o.d[0x25] = (u8)(s->n_modes - 1);
	o.d[0x26] = (u8)(s->n_temps - 1);
	o.d[0x28] = WBF_END;
	o.d[0x29] = WBF_TOGGLE;
	o.d[0x2a] = s->adv_flags;
	wbf_fix_header(o.d);
	out->data = o.d;
	out->size = o.n;
	out->mode_table = mode_table;
}

size_t wbf_grow(struct wbf_blob *b, size_t n)
{
	size_t at = b->size;
	u8 *d = realloc(b->data, b->size + n);

	if (!d)
		abort();
	b->data = d;
	b->size += n;
	return at;
}
