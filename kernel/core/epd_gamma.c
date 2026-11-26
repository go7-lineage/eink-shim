// SPDX-License-Identifier: GPL-2.0-only
#include "epd_gamma.h"

#define BLACK_MAX	127u
#define GAMMA_MIN	25u
#define GAMMA_MAX	400u
#define WHITE_MIN	128u
#define WHITE_MAX	255u
#define LOG_FRAC	28

static bool is_sep(char c)
{
	return c == ' ' || c == '\t' || c == '\n';
}

static const char *parse_u32(const char *s, u32 *v)
{
	const char *start;
	u32 n = 0;

	while (is_sep(*s))
		s++;
	start = s;
	for (; *s >= '0' && *s <= '9'; s++) {
		if (n > 9999)
			return NULL;
		n = n * 10 + (u32)(*s - '0');
	}
	if (s == start || (*s && !is_sep(*s)))
		return NULL;
	*v = n;
	return s;
}

int epd_levels_parse(const char *s, struct epd_levels *out)
{
	u32 v[3];
	size_t i;

	for (i = 0; i < ARRAY_SIZE(v); i++) {
		s = parse_u32(s, &v[i]);
		if (!s)
			return -EINVAL;
	}
	while (is_sep(*s))
		s++;
	if (*s || v[0] > BLACK_MAX || v[1] < GAMMA_MIN || v[1] > GAMMA_MAX || v[2] < WHITE_MIN || v[2] > WHITE_MAX)
		return -EINVAL;
	out->black = v[0];
	out->gamma = v[1];
	out->white = v[2];
	return 0;
}

bool epd_levels_identity(const struct epd_levels *l)
{
	return l->black == 0 && l->gamma == 100 && l->white == WHITE_MAX;
}

static u64 log2_fix(u32 n)
{
	u64 m, frac = 0;
	u32 ip = 0;
	int i;

	while (n >> (ip + 1))
		ip++;
	m = ((u64)n << LOG_FRAC) >> ip;
	for (i = LOG_FRAC - 1; i >= 0; i--) {
		m = (m * m) >> LOG_FRAC;
		if (m >= 2ull << LOG_FRAC) {
			m >>= 1;
			frac |= 1ull << i;
		}
	}
	return (u64)ip << LOG_FRAC | frac;
}

void epd_gamma_build(u8 *lut, const struct epd_levels *l)
{
	const u64 top = log2_fix(2 * WHITE_MAX);
	const u64 span = log2_fix(l->white - l->black);
	u32 i;

	for (i = 0; i < 256; i++) {
		u32 lo = 0, hi = WHITE_MAX;
		u64 drop;

		if (i <= l->black) {
			lut[i] = 0;
			continue;
		}
		if (i >= l->white) {
			lut[i] = WHITE_MAX;
			continue;
		}
		drop = (span - log2_fix(i - l->black)) * l->gamma / 100;
		while (lo < hi) {
			u32 mid = (lo + hi) / 2;

			if (log2_fix(2 * mid + 1) + drop <= top)
				lo = mid + 1;
			else
				hi = mid;
		}
		lut[i] = (u8)lo;
	}
}

bool epd_lut_build(u8 *lut, const struct epd_levels *l, bool invert)
{
	bool gamma = l && !epd_levels_identity(l);
	u32 i;

	if (!gamma && !invert)
		return false;
	if (gamma)
		epd_gamma_build(lut, l);
	else
		for (i = 0; i < 256; i++)
			lut[i] = (u8)i;
	if (invert)
		for (i = 0; i < 128; i++) {
			u8 t = lut[i];

			lut[i] = lut[255 - i];
			lut[255 - i] = t;
		}
	return true;
}
