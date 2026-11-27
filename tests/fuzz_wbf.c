// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>
#include <string.h>

#include "fixture.h"

int LLVMFuzzerTestOneInput(const u8 *data, size_t size);

static const struct epd_timing tiny = {
	.width = 16, .height = 4, .bitspanel = 16,
	.lsl = 2, .lbl = 2, .lel = 2, .ldl = 2,
	.fsl = 1, .fbl = 1, .fel = 2,
};

static unsigned int two_frames(unsigned int mode, unsigned int temp)
{
	return 1 + (mode + temp) % 2;
}

static const u8 seed_bounds[] = { 0, 25, 50 };

static const struct wbf_spec seed_spec = {
	.mode_version = FIX_VERSION,
	.lut_fmt = 4,
	.adv_flags = 0x80,
	.frame_rate = 85,
	.n_modes = FIX_MODES,
	.n_temps = 2,
	.temp_bounds = seed_bounds,
	.name = "F",
	.frames = two_frames,
	.drive = fixture_drive,
};

static void check_tables(const u8 *wbf, size_t size)
{
	struct epd_table_cfg tc = { .cut = 3, .fill_zero = true, .a2_clean_mode = 2, .a2_slot = 6, .du_slot = 1 };
	struct epd_wbf w;
	int m, t, n;

	if (epd_wbf_parse(&w, wbf, size))
		return;
	for (m = 0; m < w.n_modes; m++)
		for (t = 0; t < w.n_temps; t++) {
			size_t l2 = (size_t)w.levels * w.levels, cap = (size_t)epd_wbf_frames(&w, t, m) * l2;
			u8 *buf = malloc(cap + 1), *full = malloc(cap / 4 + 1), *part = malloc(cap / 4 + 1);

			n = epd_wbf_decode(&w, t, m, buf, cap);
			if (n != epd_wbf_frames(&w, t, m))
				__builtin_trap();
			if (n > 0)
				epd_tables4(buf, n, w.levels, m, &tc, full, part);
			free(buf);
			free(full);
			free(part);
		}
	epd_wbf_release(&w);
}

static void check_engine(const u8 *wbf, size_t size, const u8 *ops, size_t nops)
{
	struct epd_engine_cfg ec = {
		.timing = &tiny, .temp_c = 24,
		.tables = { .cut = 3, .fill_zero = true, .a2_clean_mode = 2 },
		.regal_aa = true, .supersede = true,
	};
	static u32 frame[256];
	struct epd_engine *e;
	u8 grey[64];
	size_t i;
	int err, k;

	e = epd_engine_create(&ec, wbf, size, &err);
	if (!e)
		return;
	epd_frame_template(&tiny, frame);
	for (i = 0; i + 4 <= nops && i < 4 * 64; i += 4) {
		struct epd_update u = {
			.left = ops[i] % 16, .top = ops[i] / 16 % 4,
			.width = (u16)(1 + ops[i + 1] % 16), .height = (u16)(1 + ops[i + 1] / 16 % 4),
			.mode = ops[i + 2] % 18, .full = ops[i + 2] >> 7, .repeat = ops[i + 1] >> 7,
			.marker = (u32)i,
		};

		memset(grey, ops[i + 3], sizeof(grey));
		grey[i % 64] ^= 0xf0;
		epd_engine_submit(e, &u, grey, 16);
		if (ops[i + 3] & 1)
			epd_engine_tick(e, frame);
		if (ops[i + 3] & 2)
			epd_engine_set_temp_mc(e, ops[i + 2] * 1000);
		if (ops[i + 3] & 4)
			epd_engine_set_cut(e, ops[i + 1] % 8);
	}
	for (k = 0; k < 200000 && epd_engine_busy(e); k++)
		epd_engine_tick(e, frame);
	if (epd_engine_busy(e))
		__builtin_trap();
	epd_engine_destroy(e);
}

int LLVMFuzzerTestOneInput(const u8 *data, size_t size)
{
	static struct wbf_blob seed;
	size_t nops, i;
	u8 *wbf;

	if (!seed.data)
		wbf_build(&seed_spec, &seed);
	if (size < 2)
		return 0;
	if (data[0] & 1) {
		wbf = malloc(size - 1);
		memcpy(wbf, data + 1, size - 1);
		check_tables(wbf, size - 1);
		check_engine(wbf, size - 1, data + 1, size - 1);
		free(wbf);
		return 0;
	}
	nops = data[1];
	if (2 + 4 * nops > size)
		nops = (size - 2) / 4;
	wbf = malloc(seed.size);
	memcpy(wbf, seed.data, seed.size);
	for (i = 2 + 4 * nops; i + 3 <= size; i += 3)
		wbf[((size_t)data[i] << 8 | data[i + 1]) % seed.size] = data[i + 2];
	if (data[0] & 2)
		wbf_fix_header(wbf);
	check_tables(wbf, seed.size);
	check_engine(wbf, seed.size, data + 2, 4 * nops);
	free(wbf);
	return 0;
}
