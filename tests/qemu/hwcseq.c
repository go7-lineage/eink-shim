// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

#include "kms_card.h"

#define EBC_SEND_UPDATE	0x700c

struct kms {
	int fd;
	uint32_t crtc, conn, plane, w, h, mode_blob;
	uint32_t active, mode_id, conn_crtc, fb_id, crtc_id, src_x, src_y, src_w, src_h, crtc_x, crtc_y, crtc_w,
		 crtc_h;
};

struct req {
	uint32_t objs[4], counts[4], props[16];
	uint64_t vals[16];
	uint32_t nobj, nprop;
	bool overflow;
};

struct ebc_upd {
	int32_t top, left, width, height, waveform_mode, update_mode, update_marker, temp, flag, reserved;
};

static uint32_t prop_id(int fd, uint32_t obj, uint32_t type, const char *name)
{
	uint32_t ids[64], i;
	uint64_t vals[64];
	struct drm_mode_obj_get_properties op = {
		.obj_id = obj, .obj_type = type, .count_props = 64,
		.props_ptr = (uintptr_t)ids, .prop_values_ptr = (uintptr_t)vals,
	};

	if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op))
		return 0;
	for (i = 0; i < op.count_props && i < 64; i++) {
		struct drm_mode_get_property gp = { .prop_id = ids[i] };

		if (!ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp) && !strcmp(gp.name, name))
			return ids[i];
	}
	return 0;
}

static void add(struct req *r, uint32_t obj, uint32_t prop, uint64_t val)
{
	bool new_obj = !r->nobj || r->objs[r->nobj - 1] != obj;

	if (r->nprop >= 16 || (new_obj && r->nobj >= 4)) {
		r->overflow = true;
		return;
	}
	if (new_obj) {
		r->objs[r->nobj] = obj;
		r->counts[r->nobj++] = 0;
	}
	r->counts[r->nobj - 1]++;
	r->props[r->nprop] = prop;
	r->vals[r->nprop++] = val;
}

static int commit(const struct kms *k, const struct req *r, bool modeset)
{
	struct drm_mode_atomic a = {
		.flags = modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0,
		.count_objs = r->nobj,
		.objs_ptr = (uintptr_t)r->objs,
		.count_props_ptr = (uintptr_t)r->counts,
		.props_ptr = (uintptr_t)r->props,
		.prop_values_ptr = (uintptr_t)r->vals,
	};

	if (r->overflow)
		return -E2BIG;
	return ioctl(k->fd, DRM_IOCTL_MODE_ATOMIC, &a) ? -errno : 0;
}

static uint32_t buffer(const struct kms *k, uint32_t grey, bool fill)
{
	struct drm_mode_create_dumb cd = { .width = k->w, .height = k->h, .bpp = 32 };
	struct drm_mode_fb_cmd2 fb = { .width = k->w, .height = k->h, .pixel_format = DRM_FORMAT_XRGB8888 };
	struct drm_mode_map_dumb md = { 0 };
	uint32_t *px, x, y;

	if (ioctl(k->fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd))
		return 0;
	if (fill) {
		md.handle = cd.handle;
		if (ioctl(k->fd, DRM_IOCTL_MODE_MAP_DUMB, &md))
			return 0;
		px = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, k->fd, (off_t)md.offset);
		if (px == MAP_FAILED)
			return 0;
		for (y = 0; y < cd.height; y++)
			for (x = 0; x < cd.width; x++)
				px[y * (cd.pitch / 4) + x] = grey * 0x010101u;
		(void)munmap(px, cd.size);
	}
	fb.handles[0] = cd.handle;
	fb.pitches[0] = cd.pitch;
	return ioctl(k->fd, DRM_IOCTL_MODE_ADDFB2, &fb) ? 0 : fb.fb_id;
}

static void add_plane(const struct kms *k, struct req *r, uint32_t fb)
{
	add(r, k->plane, k->fb_id, fb);
	add(r, k->plane, k->crtc_id, k->crtc);
	add(r, k->plane, k->src_x, 0);
	add(r, k->plane, k->src_y, 0);
	add(r, k->plane, k->src_w, (uint64_t)k->w << 16);
	add(r, k->plane, k->src_h, (uint64_t)k->h << 16);
	add(r, k->plane, k->crtc_x, 0);
	add(r, k->plane, k->crtc_y, 0);
	add(r, k->plane, k->crtc_w, k->w);
	add(r, k->plane, k->crtc_h, k->h);
}

static int cmd_on(const struct kms *k)
{
	struct req r = { 0 };
	uint32_t fb = buffer(k, 0, false);
	int ret;

	if (!fb)
		return -ENOMEM;
	add(&r, k->crtc, k->mode_id, k->mode_blob);
	add(&r, k->crtc, k->active, 1);
	add(&r, k->conn, k->conn_crtc, k->crtc);
	add_plane(k, &r, fb);
	ret = commit(k, &r, true);
	if (ret)
		return ret;
	memset(&r, 0, sizeof(r));
	add(&r, k->crtc, k->active, 1);
	add(&r, k->conn, k->conn_crtc, k->crtc);
	return commit(k, &r, true);
}

static int cmd_off(const struct kms *k)
{
	struct req r = { 0 };

	add(&r, k->crtc, k->active, 0);
	add(&r, k->crtc, k->mode_id, 0);
	add(&r, k->conn, k->conn_crtc, 0);
	return commit(k, &r, true);
}

static int cmd_frame(const struct kms *k, uint32_t grey)
{
	struct req r = { 0 };
	uint32_t fb = buffer(k, grey, true);

	if (!fb)
		return -ENOMEM;
	add_plane(k, &r, fb);
	return commit(k, &r, false);
}

static int cmd_ebc(const struct kms *k)
{
	struct ebc_upd u = { .width = (int32_t)k->w, .height = (int32_t)k->h, .waveform_mode = 2, .update_mode = 1 };
	int fd = open("/dev/ebc", O_RDWR | O_CLOEXEC), ret;

	if (fd < 0)
		return -errno;
	ret = ioctl(fd, EBC_SEND_UPDATE, &u) ? -errno : 0;
	close(fd);
	return ret;
}

static int setup(struct kms *k)
{
	uint32_t crtcs[4] = { 0 }, conns[4] = { 0 }, encs[4], fbs[4], planes[4] = { 0 };
	struct drm_mode_card_res res = {
		.crtc_id_ptr = (uintptr_t)crtcs, .connector_id_ptr = (uintptr_t)conns,
		.encoder_id_ptr = (uintptr_t)encs, .fb_id_ptr = (uintptr_t)fbs,
		.count_crtcs = 4, .count_connectors = 4, .count_encoders = 4, .count_fbs = 4,
	};
	struct drm_mode_get_plane_res pr = { .plane_id_ptr = (uintptr_t)planes, .count_planes = 4 };
	struct drm_set_client_cap universal = { .capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES, .value = 1 };
	struct drm_set_client_cap atomic = { .capability = DRM_CLIENT_CAP_ATOMIC, .value = 1 };
	struct drm_mode_modeinfo modes[8] = { 0 };
	struct drm_mode_get_connector gc = { 0 };
	struct drm_mode_create_blob blob = { .length = sizeof(modes[0]) };

	memset(k, 0, sizeof(*k));
	k->fd = open_card("onyx-epd");
	if (k->fd < 0)
		return fail("onyx-epd card");
	if (ioctl(k->fd, DRM_IOCTL_SET_CLIENT_CAP, &universal) || ioctl(k->fd, DRM_IOCTL_SET_CLIENT_CAP, &atomic))
		return fail("SET_CLIENT_CAP");
	if (ioctl(k->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_crtcs || !res.count_connectors)
		return fail("GETRESOURCES");
	if (ioctl(k->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) || !pr.count_planes)
		return fail("GETPLANERESOURCES");
	gc.connector_id = conns[0];
	if (ioctl(k->fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) || !gc.count_modes || gc.count_modes > 8)
		return fail("GETCONNECTOR count");
	gc.modes_ptr = (uintptr_t)modes;
	gc.count_props = 0;
	gc.count_encoders = 0;
	if (ioctl(k->fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc))
		return fail("GETCONNECTOR modes");
	blob.data = (uintptr_t)&modes[0];
	if (ioctl(k->fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob))
		return fail("CREATEPROPBLOB");
	k->crtc = crtcs[0];
	k->conn = conns[0];
	k->plane = planes[0];
	k->w = modes[0].hdisplay;
	k->h = modes[0].vdisplay;
	k->mode_blob = blob.blob_id;
	k->active = prop_id(k->fd, k->crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE");
	k->mode_id = prop_id(k->fd, k->crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID");
	k->conn_crtc = prop_id(k->fd, k->conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
	k->fb_id = prop_id(k->fd, k->plane, DRM_MODE_OBJECT_PLANE, "FB_ID");
	k->crtc_id = prop_id(k->fd, k->plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
	k->src_x = prop_id(k->fd, k->plane, DRM_MODE_OBJECT_PLANE, "SRC_X");
	k->src_y = prop_id(k->fd, k->plane, DRM_MODE_OBJECT_PLANE, "SRC_Y");
	k->src_w = prop_id(k->fd, k->plane, DRM_MODE_OBJECT_PLANE, "SRC_W");
	k->src_h = prop_id(k->fd, k->plane, DRM_MODE_OBJECT_PLANE, "SRC_H");
	k->crtc_x = prop_id(k->fd, k->plane, DRM_MODE_OBJECT_PLANE, "CRTC_X");
	k->crtc_y = prop_id(k->fd, k->plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
	k->crtc_w = prop_id(k->fd, k->plane, DRM_MODE_OBJECT_PLANE, "CRTC_W");
	k->crtc_h = prop_id(k->fd, k->plane, DRM_MODE_OBJECT_PLANE, "CRTC_H");
	if (!k->active || !k->mode_id || !k->conn_crtc || !k->fb_id || !k->crtc_id || !k->src_x || !k->src_y ||
	    !k->src_w || !k->src_h || !k->crtc_x || !k->crtc_y || !k->crtc_w || !k->crtc_h) {
		(void)fprintf(stderr, "hwcseq: missing KMS properties\n");
		return 1;
	}
	return 0;
}

static int run(const struct kms *k, const char *line)
{
	char *end;
	long v;

	if (!strcmp(line, "on"))
		return cmd_on(k);
	if (!strcmp(line, "off"))
		return cmd_off(k);
	if (!strcmp(line, "ebc"))
		return cmd_ebc(k);
	if (strncmp(line, "frame ", 6) != 0)
		return -EINVAL;
	errno = 0;
	v = strtol(line + 6, &end, 10);
	if (errno || *end || end == line + 6 || v < 0 || v > 255)
		return -EINVAL;
	return cmd_frame(k, (uint32_t)v);
}

int main(void)
{
	struct kms k;
	char line[64];
	unsigned int n = 0;

	if (setup(&k))
		return 1;
	while (fgets(line, (int)sizeof(line), stdin)) {
		int ret;

		line[strcspn(line, "\n")] = 0;
		ret = run(&k, line);
		n++;
		if (ret)
			(void)printf("hwcseq: %u err %d (%s)\n", n, -ret, line);
		else
			(void)printf("hwcseq: %u ok\n", n);
		(void)fflush(stdout);
	}
	return 0;
}
