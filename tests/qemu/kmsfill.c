// SPDX-License-Identifier: GPL-2.0-only
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

#include "kms_card.h"

int main(void)
{
	uint32_t crtcs[4] = { 0 }, conns[4] = { 0 }, encs[4], fbs[4], x, y;
	struct drm_mode_modeinfo modes[8] = { 0 };
	struct drm_mode_card_res res = {
		.crtc_id_ptr = (uintptr_t)crtcs, .connector_id_ptr = (uintptr_t)conns,
		.encoder_id_ptr = (uintptr_t)encs, .fb_id_ptr = (uintptr_t)fbs,
		.count_crtcs = 4, .count_connectors = 4, .count_encoders = 4, .count_fbs = 4,
	};
	struct drm_mode_get_connector gc = { 0 };
	struct drm_mode_create_dumb cd = { .bpp = 32 };
	struct drm_mode_fb_cmd2 fb = { .pixel_format = DRM_FORMAT_XRGB8888 };
	struct drm_mode_map_dumb md = { 0 };
	struct drm_mode_crtc sc = { 0 };
	struct drm_mode_fb_dirty_cmd dirty = { 0 };
	uint32_t *px;
	int fd = open_card("onyx-epd");

	if (fd < 0)
		return fail("onyx-epd card");
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_crtcs || !res.count_connectors)
		return fail("GETRESOURCES");
	gc.connector_id = conns[0];
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) || !gc.count_modes || gc.count_modes > 8)
		return fail("GETCONNECTOR count");
	gc.modes_ptr = (uintptr_t)modes;
	gc.count_props = 0;
	gc.count_encoders = 0;
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc))
		return fail("GETCONNECTOR modes");

	cd.width = modes[0].hdisplay;
	cd.height = modes[0].vdisplay;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd))
		return fail("CREATE_DUMB");
	md.handle = cd.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md))
		return fail("MAP_DUMB");
	px = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)md.offset);
	if (px == MAP_FAILED)
		return fail("mmap");
	for (y = 0; y < cd.height; y++)
		for (x = 0; x < cd.width; x++) {
			uint32_t g = x * 255 / (cd.width - 1);

			px[y * (cd.pitch / 4) + x] = g << 16 | g << 8 | g;
		}

	fb.width = cd.width;
	fb.height = cd.height;
	fb.handles[0] = cd.handle;
	fb.pitches[0] = cd.pitch;
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb))
		return fail("ADDFB2");
	sc.crtc_id = crtcs[0];
	sc.fb_id = fb.fb_id;
	sc.set_connectors_ptr = (uintptr_t)conns;
	sc.count_connectors = 1;
	sc.mode = modes[0];
	sc.mode_valid = 1;
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &sc))
		return fail("SETCRTC");
	dirty.fb_id = fb.fb_id;
	if (ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty))
		return fail("DIRTYFB");
	printf("kmsfill: showing a %ux%u grey ramp\n", cd.width, cd.height);
	(void)fflush(stdout);
	for (;;)
		pause();
}
