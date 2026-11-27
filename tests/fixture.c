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

unsigned int fixture_plain_frames(unsigned int slot, unsigned int temp)
{
	static const u8 n[FIX_MODES] = { 2, 4, 6, 2, 5, 1, 3, 1, 2, 5, 5 };

	(void)temp;
	return n[slot];
}

u8 fixture_plain_drive(unsigned int slot, unsigned int temp, unsigned int frame, unsigned int nw, unsigned int old)
{
	if (frame + 1 >= fixture_plain_frames(slot, temp))
		return 0;
	switch (slot) {
	case 1:
		return nw == 30 && old != 30 ? 2 : nw == 0 && old != 0 ? 1 : 0;
	case 6:
		return nw == 30 && old == 0 ? 2 : nw == 0 && old == 30 ? 1 : 0;
	case 9:
		if (nw == 14 && old == 0)
			return 0;
		break;
	default:
		break;
	}
	return nw > old ? 2 : nw < old ? 1 : frame == 0 ? 1 : 2;
}

const struct wbf_blob *fixture_plain_wbf(void)
{
	static const u8 plain_bounds[] = { 0, 50 };
	static struct wbf_blob blob;
	struct wbf_spec s = fixture_spec;

	if (!blob.data) {
		s.n_temps = 1;
		s.temp_bounds = plain_bounds;
		s.frames = fixture_plain_frames;
		s.drive = fixture_plain_drive;
		wbf_build(&s, &blob);
	}
	return &blob;
}
