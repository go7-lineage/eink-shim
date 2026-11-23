// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>
#include <string.h>

#include "epd_tables.h"
#include "test.h"

#define NFR	6
#define A2_SLOT	6
#define DU_SLOT	1

static u8 src[NFR * 1024], full[NFR * 256], part[NFR * 256];

static u8 src_at(int f, int nw, int old)
{
	return src[f * 1024 + 2 * nw * 32 + 2 * old];
}

static void fill_src(void)
{
	size_t i;

	for (i = 0; i < sizeof(src); i++)
		src[i] = (u8)(i * 2654435761u >> 28);
	memset(full, 0xee, sizeof(full));
	memset(part, 0xee, sizeof(part));
}

static struct epd_table_cfg cfg(u8 cut, bool fill_zero, u8 clean)
{
	struct epd_table_cfg c = { .cut = cut, .fill_zero = fill_zero, .a2_clean_mode = clean,
				   .a2_slot = A2_SLOT, .du_slot = DU_SLOT };

	return c;
}

static void check_plain(int n, int zero_frame)
{
	int f, nw, old;

	for (f = 0; f < n; f++)
		for (nw = 0; nw < 16; nw++)
			for (old = 0; old < 16; old++) {
				u8 want = f == zero_frame ? 0 : src_at(f, nw, old);

				CHECK_EQ(full[f * 256 + (nw << 4 | old)], want);
				CHECK_EQ(part[f * 256 + (nw << 4 | old)], nw == old ? 0 : want);
			}
}

TEST(tables_rejects_bad_input)
{
	struct epd_table_cfg c = cfg(0, false, 0);

	fill_src();
	CHECK_EQ(epd_tables4(src, NFR, 16, 2, &c, full, part), -EINVAL);
	CHECK_EQ(epd_tables4(src, 0, 32, 2, &c, full, part), -EINVAL);
}

TEST(tables_take_even_levels)
{
	struct epd_table_cfg c = cfg(3, true, 2);

	fill_src();
	CHECK_EQ(epd_tables4(src, NFR, 32, 2, &c, full, part), NFR);
	check_plain(NFR, -1);
}

TEST(tables_cut_applies_to_a2_and_du_only)
{
	struct epd_table_cfg c = cfg(2, false, 0);

	fill_src();
	CHECK_EQ(epd_tables4(src, NFR, 32, DU_SLOT, &c, full, part), NFR - 2);
	check_plain(NFR - 2, -1);
	CHECK_EQ(full[(NFR - 2) * 256], 0xee);
	fill_src();
	CHECK_EQ(epd_tables4(src, NFR, 32, A2_SLOT, &c, full, part), NFR - 2);
	check_plain(NFR - 2, -1);
}

TEST(tables_cut_keeps_one_frame)
{
	struct epd_table_cfg c = cfg(200, false, 0);

	fill_src();
	CHECK_EQ(epd_tables4(src, NFR, 32, DU_SLOT, &c, full, part), 1);
	check_plain(1, -1);
}

TEST(tables_fill_zero_blanks_last_kept_frame)
{
	struct epd_table_cfg c = cfg(2, true, 0);

	fill_src();
	CHECK_EQ(epd_tables4(src, NFR, 32, DU_SLOT, &c, full, part), NFR - 2);
	check_plain(NFR - 2, NFR - 3);
	c.cut = 0;
	fill_src();
	CHECK_EQ(epd_tables4(src, NFR, 32, DU_SLOT, &c, full, part), NFR);
	check_plain(NFR, -1);
}

static bool in(const u8 *set, size_t n, int v)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (set[i] == v)
			return true;
	return false;
}

TEST(tables_a2_clean_mode_2_patches_full_table)
{
	static const u8 to_black[] = { 1, 2, 3, 8, 12, 13, 14 }, to_white[] = { 1, 2, 3, 7, 12, 13, 14 };
	struct epd_table_cfg c = cfg(0, false, 2);
	int f, nw, old, n;

	for (n = 1; n <= NFR; n++) {
		fill_src();
		CHECK_EQ(epd_tables4(src, n, 32, A2_SLOT, &c, full, part), n);
		for (f = 0; f < n; f++)
			for (nw = 0; nw < 16; nw++)
				for (old = 0; old < 16; old++) {
					u8 want = src_at(f, nw, old);

					CHECK_EQ(part[f * 256 + (nw << 4 | old)], nw == old ? 0 : want);
					if (n > 1 && nw == 0 && f < n - 1 && in(to_black, sizeof(to_black), old))
						want = 1;
					if (n > 1 && nw == 15 && f < n - 1 && in(to_white, sizeof(to_white), old))
						want = 2;
					if (n > 1 && nw == 15 && old == 8 && f >= n - 4 && f <= n - 2)
						want = 2;
					if (n > 1 && nw == 0 && old == 7 && f >= n - 3 && f <= n - 2)
						want = 1;
					CHECK_EQ(full[f * 256 + (nw << 4 | old)], want);
				}
		if (n < NFR)
			CHECK_EQ(full[n * 256], 0xee);
	}
}

TEST(tables_a2_clean_mode_2_stays_inside_exact_buffers)
{
	struct epd_table_cfg c = cfg(0, false, 2);
	int n;

	fill_src();
	for (n = 1; n <= 4; n++) {
		u8 *f = malloc((size_t)n * 256), *p = malloc((size_t)n * 256);

		CHECK_EQ(epd_tables4(src, n, 32, A2_SLOT, &c, f, p), n);
		free(f);
		free(p);
	}
}

TEST(tables_a2_clean_mode_1_unsupported)
{
	struct epd_table_cfg c = cfg(0, false, 1);

	fill_src();
	CHECK_EQ(epd_tables4(src, NFR, 32, A2_SLOT, &c, full, part), -EOPNOTSUPP);
	CHECK_EQ(epd_tables4(src, NFR, 32, DU_SLOT, &c, full, part), NFR);
}

TEST(tables_regal_fixup)
{
	static u8 f[2 * 1024], orig[2 * 1024];
	int variant, i, k;

	for (variant = 0; variant < 3; variant++) {
		for (i = 0; i < 2 * 1024; i++)
			f[i] = orig[i] = (u8)(1 + i % 3);
		epd_regal_fixup(f, 2, variant);
		for (i = 0; i < 2; i++) {
			const u8 *g = f + i * 1024, *o = orig + i * 1024;

			CHECK_EQ(g[30 * 32 + 30], 0);
			CHECK_EQ(g[30 * 32 + 31], o[31 * 32 + 31]);
			CHECK_EQ(g[30 * 32 + 29], o[29 * 32 + 29]);
			CHECK_EQ(g[0], variant == 2 ? o[0] : 0);
			CHECK_EQ(g[1], o[1 * 32]);
			CHECK_EQ(g[3], o[3 * 32]);
			for (k = 1; k < 32; k++) {
				u8 want = k == 30 ? 0 : o[k * 33];

				if (variant == 1)
					want = 0;
				CHECK_EQ(g[k * 33], want);
			}
			CHECK_EQ(g[5 * 32 + 9], o[5 * 32 + 9]);
		}
	}
}

TEST(tables_mode14_sequence)
{
	static const u8 want[30] = { 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1,
				     2, 2, 2, 2, 2, 2, 2, 2, 0 };
	static u8 f[30 * 1024];
	int i;

	CHECK_EQ(epd_mode14_frames(f, sizeof(f) - 1), -ENOSPC);
	CHECK_EQ(epd_mode14_frames(f, sizeof(f)), 30);
	for (i = 0; i < 30; i++) {
		CHECK_EQ(f[i * 1024], want[i]);
		CHECK_EQ(f[i * 1024 + 1023], want[i]);
	}
}
