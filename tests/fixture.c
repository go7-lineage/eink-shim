// SPDX-License-Identifier: GPL-2.0-only
#include "fixture.h"

static const u8 bounds[FIX_TEMPS + 1] = { 0, 20, 30, 50 };

unsigned int fixture_frames(unsigned int slot, unsigned int temp)
{
	static const u8 n[FIX_MODES] = { 4, 3, 5, 2, 4, 2, 6, 2, 2, 4, 1 };

	if (slot == 9 && temp == 2)
		return 0;
	return n[slot] + (slot == 2 ? temp : 0);
}

u8 fixture_drive(unsigned int slot, unsigned int temp, unsigned int frame, unsigned int nw, unsigned int old)
{
	return (u8)((slot * 7 + temp * 3 + frame * 5 + nw * 11 + old * 13 + nw * old) % 3);
}

const struct wbf_spec fixture_spec = {
	.mode_version = FIX_VERSION,
	.lut_fmt = 4,
	.adv_flags = 0x82,
	.frame_rate = 85,
	.n_modes = FIX_MODES,
	.n_temps = FIX_TEMPS,
	.temp_bounds = bounds,
	.name = "TEST_WAVEFORM",
	.frames = fixture_frames,
	.drive = fixture_drive,
};

const struct epd_timing fixture_timing = {
	.width = 48, .height = 6, .bitspanel = 16,
	.lsl = 2, .lbl = 2, .lel = 2, .ldl = 6,
	.fsl = 1, .fbl = 1, .fel = 2,
};

const struct wbf_blob *fixture_wbf(void)
{
	static struct wbf_blob blob;

	if (!blob.data)
		wbf_build(&fixture_spec, &blob);
	return &blob;
}
