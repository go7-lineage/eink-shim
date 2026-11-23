/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FIXTURE_H
#define FIXTURE_H

#include "epd_engine.h"
#include "wbf_build.h"

#define FIX_MODES	11
#define FIX_TEMPS	3
#define FIX_VERSION	88

extern const struct wbf_spec fixture_spec;
extern const struct epd_timing fixture_timing;

unsigned int fixture_frames(unsigned int slot, unsigned int temp);
u8 fixture_drive(unsigned int slot, unsigned int temp, unsigned int frame, unsigned int nw, unsigned int old);
const struct wbf_blob *fixture_wbf(void);

#endif
