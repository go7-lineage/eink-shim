// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>
#include <string.h>

#include "fixture.h"
#include "test.h"

static unsigned int one_frame(unsigned int mode, unsigned int temp)
{
	(void)mode;
	(void)temp;
	return 1;
}

static const u8 small_bounds[] = { 0, 50 };

static const struct wbf_spec small_spec = {
	.mode_version = 0,
	.lut_fmt = 4,
	.frame_rate = 85,
	.n_modes = 2,
	.n_temps = 1,
	.temp_bounds = small_bounds,
	.name = "S",
	.frames = one_frame,
	.drive = fixture_drive,
};

static u8 *exact_copy(const u8 *d, size_t n)
{
	u8 *c = malloc(n ? n : 1);

	if (!c)
		abort();
	memcpy(c, d, n);
	return c;
}

static int parse_copy(const u8 *d, size_t n)
{
	u8 *c = exact_copy(d, n);
	struct epd_wbf w;
	int ret = epd_wbf_parse(&w, c, n);

	if (!ret)
		epd_wbf_release(&w);
	free(c);
	return ret;
}

TEST(wbf_parse_header)
{
	const struct wbf_blob *b = fixture_wbf();
	struct epd_wbf w;

	CHECK_EQ(epd_wbf_parse(&w, b->data, b->size), 0);
	CHECK_EQ(w.mode_version, FIX_VERSION);
	CHECK_EQ(w.frame_rate, 85);
	CHECK_EQ(w.levels, 32);
	CHECK_EQ(w.n_modes, FIX_MODES);
	CHECK_EQ(w.n_temps, FIX_TEMPS);
	CHECK_EQ(w.end_byte, WBF_END);
	CHECK_EQ(w.toggle_byte, WBF_TOGGLE);
	CHECK_EQ(w.adv_flags, 0x82);
	CHECK_MEM(w.temp_bounds, fixture_spec.temp_bounds, FIX_TEMPS + 1);
	CHECK(!strcmp(w.name, "TEST_WAVEFORM"));
	epd_wbf_release(&w);
	CHECK(!w.block && !w.nframes);
}

TEST(wbf_decode_matches_generator)
{
	const struct wbf_blob *b = fixture_wbf();
	struct epd_wbf w;
	unsigned int m, t, f, nw, old;

	CHECK_EQ(epd_wbf_parse(&w, b->data, b->size), 0);
	for (m = 0; m < FIX_MODES; m++)
		for (t = 0; t < FIX_TEMPS; t++) {
			unsigned int n = fixture_frames(m, t);
			size_t cap = (size_t)n * 1024;
			u8 *out = malloc(cap + 1);

			CHECK_EQ(epd_wbf_frames(&w, (int)t, (int)m), n);
			CHECK_EQ(epd_wbf_decode(&w, (int)t, (int)m, out, cap), n);
			for (f = 0; f < n; f++)
				for (nw = 0; nw < 32; nw++)
					for (old = 0; old < 32; old++)
						if (out[f * 1024 + nw * 32 + old] != fixture_drive(m, t, f, nw, old)) {
							CHECK_EQ(out[f * 1024 + nw * 32 + old],
								 fixture_drive(m, t, f, nw, old));
							f = n;
							nw = old = 32;
						}
			if (n)
				CHECK_EQ(epd_wbf_decode(&w, (int)t, (int)m, out, cap - 1), -ENOSPC);
			free(out);
		}
	epd_wbf_release(&w);
}

TEST(wbf_decode_rejects_bad_index)
{
	const struct wbf_blob *b = fixture_wbf();
	struct epd_wbf w;
	u8 out[4];

	CHECK_EQ(epd_wbf_parse(&w, b->data, b->size), 0);
	CHECK_EQ(epd_wbf_decode(&w, -1, 0, out, sizeof(out)), -EINVAL);
	CHECK_EQ(epd_wbf_decode(&w, FIX_TEMPS, 0, out, sizeof(out)), -EINVAL);
	CHECK_EQ(epd_wbf_decode(&w, 0, -1, out, sizeof(out)), -EINVAL);
	CHECK_EQ(epd_wbf_decode(&w, 0, FIX_MODES, out, sizeof(out)), -EINVAL);
	epd_wbf_release(&w);
}

TEST(wbf_sixteen_levels)
{
	struct wbf_spec s = small_spec;
	struct wbf_blob b;
	struct epd_wbf w;

	s.lut_fmt = 0;
	wbf_build(&s, &b);
	CHECK_EQ(epd_wbf_parse(&w, b.data, b.size), 0);
	CHECK_EQ(w.levels, 16);
	CHECK_EQ(epd_wbf_frames(&w, 0, 1), 1);
	epd_wbf_release(&w);
	free(b.data);
}

TEST(wbf_version_0x90_has_32_levels)
{
	struct wbf_spec s = small_spec;
	struct wbf_blob b;
	struct epd_wbf w;

	s.lut_fmt = 0;
	s.mode_version = 0x90;
	wbf_build(&s, &b);
	CHECK_EQ(epd_wbf_parse(&w, b.data, b.size), 0);
	CHECK_EQ(w.levels, 32);
	epd_wbf_release(&w);
	free(b.data);
}

TEST(wbf_frame_rate_bcd_in_revision_2)
{
	struct wbf_blob b;
	struct epd_wbf w;

	wbf_build(&small_spec, &b);
	b.data[0x16] = 2;
	b.data[0x17] = 0x85;
	b.data[0x18] = 1;
	wbf_fix_header(b.data);
	CHECK_EQ(epd_wbf_parse(&w, b.data, b.size), 0);
	CHECK_EQ(w.frame_rate, 85);
	epd_wbf_release(&w);
	free(b.data);
}

TEST(wbf_rejects_short_and_bad_checksums)
{
	struct wbf_blob b;
	struct epd_wbf w;
	u8 *d;

	wbf_build(&small_spec, &b);
	d = b.data;
	CHECK_EQ(epd_wbf_parse(&w, d, 0x2f), -EINVAL);
	d[9] ^= 1;
	CHECK_EQ(epd_wbf_parse(&w, d, b.size), -EINVAL);
	d[9] ^= 1;
	d[0x2b] ^= 1;
	CHECK_EQ(epd_wbf_parse(&w, d, b.size), -EINVAL);
	d[0x2b] ^= 1;
	d[0x30] ^= 1;
	CHECK_EQ(epd_wbf_parse(&w, d, b.size), -EINVAL);
	d[0x30] ^= 1;
	d[b.mode_table + 3] ^= 1;
	CHECK_EQ(epd_wbf_parse(&w, d, b.size), -EINVAL);
	d[b.mode_table + 3] ^= 1;
	d[wbf_temp_table(&b, 1) + 3] ^= 1;
	CHECK_EQ(epd_wbf_parse(&w, d, b.size), -EINVAL);
	d[wbf_temp_table(&b, 1) + 3] ^= 1;
	d[b.size - 1] ^= 1;
	CHECK_EQ(epd_wbf_parse(&w, d, b.size), -EINVAL);
	d[b.size - 1] ^= 1;
	CHECK_EQ(epd_wbf_parse(&w, d, b.size), 0);
	epd_wbf_release(&w);
	free(d);
}

TEST(wbf_rejects_pointers_outside_file)
{
	struct wbf_blob b;
	struct epd_wbf w;

	wbf_build(&small_spec, &b);
	wbf_set_ptr(b.data, b.mode_table + 4, b.size - 3);
	CHECK_EQ(epd_wbf_parse(&w, b.data, b.size), -EINVAL);
	wbf_set_ptr(b.data, b.mode_table + 4, 0xffffff);
	CHECK_EQ(epd_wbf_parse(&w, b.data, b.size), -EINVAL);
	free(b.data);

	wbf_build(&small_spec, &b);
	wbf_set_ptr(b.data, wbf_temp_table(&b, 1), b.size);
	CHECK_EQ(epd_wbf_parse(&w, b.data, b.size), -EINVAL);
	free(b.data);
}

static size_t append_block(struct wbf_blob *b, const u8 *stream, size_t n)
{
	size_t at = wbf_grow(b, n);

	memcpy(b->data + at, stream, n);
	wbf_set_ptr(b->data, wbf_temp_table(b, 1), at);
	return at;
}

TEST(wbf_accepts_a_table_at_the_end_of_the_file)
{
	struct wbf_blob b;
	struct epd_wbf w;
	size_t block, at;

	wbf_build(&small_spec, &b);
	block = wbf_block(&b, 1, 0);
	at = wbf_grow(&b, 4);
	wbf_set_ptr(b.data, at, block);
	wbf_set_ptr(b.data, b.mode_table + 4, at);
	CHECK_EQ(parse_copy(b.data, b.size), 0);
	CHECK_EQ(epd_wbf_parse(&w, b.data, b.size - 1), -EINVAL);
	free(b.data);
}

TEST(wbf_rejects_partial_frame)
{
	const u8 stream[] = { 0x00, 0x7f, WBF_END, (u8)(0x7f + WBF_END) };
	struct wbf_blob b;
	struct epd_wbf w;

	wbf_build(&small_spec, &b);
	append_block(&b, stream, sizeof(stream));
	CHECK_EQ(epd_wbf_parse(&w, b.data, b.size), -EINVAL);
	free(b.data);
}

TEST(wbf_accepts_whole_frame_of_runs)
{
	const u8 stream[] = { 0x55, 0xff, WBF_END, (u8)(0x55 + 0xff + WBF_END) };
	struct wbf_blob b;
	struct epd_wbf w;
	u8 out[1024], want[1024];

	wbf_build(&small_spec, &b);
	append_block(&b, stream, sizeof(stream));
	CHECK_EQ(epd_wbf_parse(&w, b.data, b.size), 0);
	CHECK_EQ(epd_wbf_decode(&w, 0, 1, out, sizeof(out)), 1);
	memset(want, 1, sizeof(want));
	CHECK_MEM(out, want, sizeof(out));
	epd_wbf_release(&w);
	free(b.data);
}

TEST(wbf_literal_bytes_between_toggles)
{
	u8 stream[2 + 256 + 2], want[1024], out[1024], total = 0;
	struct wbf_blob b;
	struct epd_wbf w;
	size_t i;

	stream[0] = WBF_TOGGLE;
	for (i = 0; i < 256; i++) {
		stream[1 + i] = (u8)(i % 3 * 0x55);
		memset(want + 4 * i, (int)(i % 3), 4);
	}
	stream[257] = WBF_TOGGLE;
	for (i = 0; i < 258; i++)
		total = (u8)(total + stream[i]);
	stream[258] = WBF_END;
	stream[259] = (u8)(total + WBF_END);
	wbf_build(&small_spec, &b);
	append_block(&b, stream, sizeof(stream));
	CHECK_EQ(epd_wbf_parse(&w, b.data, b.size), 0);
	CHECK_EQ(epd_wbf_decode(&w, 0, 1, out, sizeof(out)), 1);
	CHECK_MEM(out, want, sizeof(out));
	epd_wbf_release(&w);
	free(b.data);
}

TEST(wbf_rejects_missing_end_and_run_cut_short)
{
	const u8 no_end[] = { 0x00, 0xff };
	const u8 half_run[] = { 0x00 };
	const u8 no_check[] = { 0x00, 0xff, WBF_END };
	struct wbf_blob b;
	struct epd_wbf w;

	wbf_build(&small_spec, &b);
	append_block(&b, no_end, sizeof(no_end));
	CHECK_EQ(epd_wbf_parse(&w, b.data, b.size), -EINVAL);
	free(b.data);

	wbf_build(&small_spec, &b);
	append_block(&b, half_run, sizeof(half_run));
	CHECK_EQ(epd_wbf_parse(&w, b.data, b.size), -EINVAL);
	free(b.data);

	wbf_build(&small_spec, &b);
	append_block(&b, no_check, sizeof(no_check));
	CHECK_EQ(epd_wbf_parse(&w, b.data, b.size), -EINVAL);
	free(b.data);
}

TEST(wbf_frame_limit)
{
	size_t pairs = (size_t)EPD_MAX_FRAMES + 1, i;
	u8 *stream = calloc(1, 2 * pairs + 2), total = 0;
	struct wbf_blob b;
	struct epd_wbf w;

	for (i = 0; i < pairs; i++) {
		stream[2 * i + 1] = 0xff;
		total = (u8)(total + 0xff);
	}
	stream[2 * pairs] = WBF_END;
	stream[2 * pairs + 1] = (u8)(total + WBF_END);

	wbf_build(&small_spec, &b);
	append_block(&b, stream, 2 * pairs + 2);
	CHECK_EQ(epd_wbf_parse(&w, b.data, b.size), -E2BIG);
	free(b.data);

	stream[2 * pairs - 2] = WBF_END;
	stream[2 * pairs - 1] = (u8)(total - 0xff + WBF_END);
	wbf_build(&small_spec, &b);
	append_block(&b, stream, 2 * pairs);
	CHECK_EQ(epd_wbf_parse(&w, b.data, b.size), 0);
	CHECK_EQ(epd_wbf_frames(&w, 0, 1), EPD_MAX_FRAMES);
	epd_wbf_release(&w);
	free(b.data);
	free(stream);
}

TEST(wbf_every_truncation_is_safe)
{
	struct wbf_blob b;
	size_t n;

	wbf_build(&small_spec, &b);
	for (n = 0; n < b.size; n++)
		CHECK(parse_copy(b.data, n) < 0);
	CHECK_EQ(parse_copy(b.data, b.size), 0);
	free(b.data);
}

TEST(wbf_every_byte_flip_is_safe)
{
	struct wbf_blob b;
	size_t i;
	int m;

	wbf_build(&small_spec, &b);
	for (i = 0; i < b.size; i++) {
		u8 *c = exact_copy(b.data, b.size);
		struct epd_wbf w;

		c[i] ^= 0xff;
		if (i < 0x30)
			wbf_fix_header(c);
		if (!epd_wbf_parse(&w, c, b.size)) {
			for (m = 0; m < w.n_modes; m++) {
				size_t cap = (size_t)epd_wbf_frames(&w, 0, m) * w.levels * w.levels;
				u8 *out = malloc(cap ? cap : 1);

				CHECK_EQ(epd_wbf_decode(&w, 0, m, out, cap), epd_wbf_frames(&w, 0, m));
				free(out);
			}
			epd_wbf_release(&w);
		}
		free(c);
	}
	free(b.data);
}

TEST(wbf_temp_index)
{
	const struct wbf_blob *b = fixture_wbf();
	struct epd_wbf w;

	CHECK_EQ(epd_wbf_parse(&w, b->data, b->size), 0);
	CHECK_EQ(epd_temp_index(&w, -5), 0);
	CHECK_EQ(epd_temp_index(&w, 0), 0);
	CHECK_EQ(epd_temp_index(&w, 19), 0);
	CHECK_EQ(epd_temp_index(&w, 20), 1);
	CHECK_EQ(epd_temp_index(&w, 29), 1);
	CHECK_EQ(epd_temp_index(&w, 30), 2);
	CHECK_EQ(epd_temp_index(&w, 49), 2);
	CHECK_EQ(epd_temp_index(&w, 50), 2);
	CHECK_EQ(epd_temp_index(&w, 100), 2);
	epd_wbf_release(&w);
}

TEST(wbf_temp_index_hyst)
{
	const struct wbf_blob *b = fixture_wbf();
	struct epd_wbf w;

	CHECK_EQ(epd_wbf_parse(&w, b->data, b->size), 0);
	CHECK_EQ(epd_temp_index_hyst(&w, -1, 19999), 0);
	CHECK_EQ(epd_temp_index_hyst(&w, -1, 20000), 1);
	CHECK_EQ(epd_temp_index_hyst(&w, -1, -40000), 0);
	CHECK_EQ(epd_temp_index_hyst(&w, 3, 25000), 1);
	CHECK_EQ(epd_temp_index_hyst(&w, 0, 20000), 0);
	CHECK_EQ(epd_temp_index_hyst(&w, 0, 20999), 0);
	CHECK_EQ(epd_temp_index_hyst(&w, 0, 21000), 1);
	CHECK_EQ(epd_temp_index_hyst(&w, 0, 45000), 2);
	CHECK_EQ(epd_temp_index_hyst(&w, 1, 19000), 1);
	CHECK_EQ(epd_temp_index_hyst(&w, 1, 18999), 0);
	CHECK_EQ(epd_temp_index_hyst(&w, 1, 30999), 1);
	CHECK_EQ(epd_temp_index_hyst(&w, 1, 31000), 2);
	CHECK_EQ(epd_temp_index_hyst(&w, 2, 29000), 2);
	CHECK_EQ(epd_temp_index_hyst(&w, 2, 5000), 0);
	CHECK_EQ(epd_temp_index_hyst(&w, 2, 200000), 2);
	CHECK_EQ(epd_temp_index_hyst(&w, 0, -40000), 0);
	epd_wbf_release(&w);
}

TEST(wbf_mode_slot)
{
	static const int go7[17] = { 0, 1, 2, 255, 6, 4, 255, 255, 255, 255, 255, 255, 255, 255, 0, 9, 10 };
	int m;

	for (m = 0; m < 17; m++)
		CHECK_EQ(epd_mode_slot(FIX_VERSION, m), go7[m]);
	CHECK_EQ(epd_mode_slot(FIX_VERSION, 17), 0xff);
	CHECK_EQ(epd_mode_slot(FIX_VERSION, -1), 0xff);
	CHECK_EQ(epd_mode_slot(FIX_VERSION, 0xff), 0xff);
	CHECK_EQ(epd_mode_slot(0, 7), 7);
	CHECK_EQ(epd_mode_slot(0, 14), 0);
	CHECK_EQ(epd_mode_slot(25, 15), 9);
	CHECK(epd_mode_is_regal(5) && epd_mode_is_regal(15) && epd_mode_is_regal(16));
	CHECK(!epd_mode_is_regal(2) && !epd_mode_is_regal(4));
	CHECK(epd_mode_has_aa(5) && epd_mode_has_aa(16));
	CHECK(!epd_mode_has_aa(15) && !epd_mode_has_aa(2) && !epd_mode_has_aa(4));
}

TEST(wbf_fixup_variant)
{
	struct epd_wbf w = { .mode_version = FIX_VERSION, .adv_flags = 0x80 };

	CHECK_EQ(epd_wbf_fixup_variant(&w, 4), 0);
	CHECK_EQ(epd_wbf_fixup_variant(&w, 9), 1);
	CHECK_EQ(epd_wbf_fixup_variant(&w, 10), 2);
	CHECK_EQ(epd_wbf_fixup_variant(&w, 2), -1);
	w.adv_flags = 0;
	CHECK_EQ(epd_wbf_fixup_variant(&w, 10), -1);
}
