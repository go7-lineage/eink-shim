// SPDX-License-Identifier: GPL-2.0-only

#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/pm_wakeup.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_property.h>
#include <drm/drm_vblank.h>

#include "onyx_epd_drv.h"
#include "onyx_epd_uapi.h"

bool epd_param_ui = true;
module_param_named(ui, epd_param_ui, bool, 0644);
MODULE_PARM_DESC(ui, "refresh policy for commits without EPD_UPDATES");

bool epd_param_settle_full;
module_param_named(settle_full, epd_param_settle_full, bool, 0644);
MODULE_PARM_DESC(settle_full, "redraw after fast frames as a full update, with a flash, instead of a partial one");

struct onyx_plane_state {
	struct drm_shadow_plane_state base;
	struct drm_property_blob *updates;
};

static struct onyx_plane_state *to_onyx_plane_state(struct drm_plane_state *s)
{
	return container_of(to_drm_shadow_plane_state(s), struct onyx_plane_state, base);
}

static const u32 epd_formats[] = {
	DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888,
};

bool epd_fb_bgr(const struct drm_framebuffer *fb)
{
	return fb->format->format == DRM_FORMAT_ARGB8888 || fb->format->format == DRM_FORMAT_XRGB8888;
}

static void epd_plane_reset(struct drm_plane *plane)
{
	struct onyx_plane_state *s;

	if (plane->state) {
		plane->funcs->atomic_destroy_state(plane, plane->state);
		plane->state = NULL;
	}
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (s)
		__drm_gem_reset_shadow_plane(plane, &s->base);
}

static struct drm_plane_state *epd_plane_duplicate(struct drm_plane *plane)
{
	struct onyx_plane_state *s;

	if (!plane->state)
		return NULL;
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return NULL;
	__drm_gem_duplicate_shadow_plane_state(plane, &s->base);
	s->updates = NULL;
	return &s->base.base;
}

static void epd_plane_destroy_state(struct drm_plane *plane, struct drm_plane_state *state)
{
	struct onyx_plane_state *s = to_onyx_plane_state(state);

	drm_property_blob_put(s->updates);
	__drm_gem_destroy_shadow_plane_state(&s->base);
	kfree(s);
}

static int epd_plane_set_property(struct drm_plane *plane, struct drm_plane_state *state,
				  struct drm_property *property, u64 val)
{
	struct onyx_epd *p = to_epd(plane->dev);
	struct onyx_plane_state *s = to_onyx_plane_state(state);
	bool replaced;

	if (property != p->updates_prop)
		return -EINVAL;
	return drm_property_replace_blob_from_id(plane->dev, &s->updates, val, -1,
						 sizeof(struct onyx_epdc_upd), &replaced);
}

static int epd_plane_get_property(struct drm_plane *plane, const struct drm_plane_state *state,
				  struct drm_property *property, u64 *val)
{
	struct onyx_epd *p = to_epd(plane->dev);
	struct onyx_plane_state *s = to_onyx_plane_state((struct drm_plane_state *)state);

	if (property != p->updates_prop)
		return -EINVAL;
	*val = s->updates ? s->updates->base.id : 0;
	return 0;
}

static const struct drm_plane_funcs epd_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = epd_plane_reset,
	.atomic_duplicate_state = epd_plane_duplicate,
	.atomic_destroy_state = epd_plane_destroy_state,
	.atomic_set_property = epd_plane_set_property,
	.atomic_get_property = epd_plane_get_property,
};

static int epd_plane_atomic_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct drm_plane_state *ps = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *cs = NULL;

	if (ps->crtc)
		cs = drm_atomic_get_new_crtc_state(state, ps->crtc);
	return drm_atomic_helper_check_plane_state(ps, cs, DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING,
						   false, true);
}

static bool epd_crtc_turning_on(struct drm_atomic_state *state, struct drm_crtc *crtc)
{
	struct drm_crtc_state *old_cs, *new_cs;

	if (!crtc)
		return false;
	old_cs = drm_atomic_get_old_crtc_state(state, crtc);
	new_cs = drm_atomic_get_new_crtc_state(state, crtc);
	return old_cs && new_cs && new_cs->active && !old_cs->active;
}

static void epd_plane_atomic_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct onyx_epd *p = to_epd(plane->dev);
	struct drm_plane_state *ps = drm_atomic_get_new_plane_state(state, plane);
	struct drm_plane_state *old = drm_atomic_get_old_plane_state(state, plane);
	struct onyx_plane_state *s = to_onyx_plane_state(ps);
	struct drm_framebuffer *fb = ps->fb;
	enum epd_sleep_action sf;
	bool turning_on;
	const u8 *vaddr, *fast;
	int idx;

	if (!fb || !ps->visible || drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE))
		return;
	if (!drm_dev_enter(plane->dev, &idx))
		goto out;
	vaddr = s->base.data[0].vaddr;
	turning_on = epd_crtc_turning_on(state, ps->crtc);
	if (!turning_on)
		epd_link_hint(p);
	mutex_lock(&p->in_lock);
	sf = epd_sleep_frame(&p->sleep, turning_on);
	if (sf == EPD_SLEEP_SKIP)
		goto unlock;
	fast = epd_fb_cached(p, fb);
	if (fast) {
		vaddr = fast;
		p->ui_cached++;
	}
	if (sf == EPD_SLEEP_FULL) {
		struct epd_request r;

		epd_ui_gc_request(&r, EPD_W, EPD_H);
		epd_submit(p, &r, 1, vaddr, fb->pitches[0], epd_fb_bgr(fb));
		if (p->ui_ok && fb->format->cpp[0] == 4)
			epd_ui_seed(&p->ui, vaddr, fb->pitches[0], fb->obj[0]->size - fb->offsets[0], epd_now_ms());
	} else if (s->updates) {
		const struct onyx_epdc_upd *u = s->updates->data;
		size_t i, n = min_t(size_t, s->updates->length / sizeof(*u), 8);

		for (i = 0; i < n; i++)
			epd_request(p, &u[i], vaddr, fb->pitches[0], epd_fb_bgr(fb));
		if (p->ui_ok)
			epd_ui_invalidate(&p->ui);
	} else if (epd_param_ui && p->ui_ok) {
		struct epd_request r[EPD_UI_MAX_RECTS];
		int cut;

		u64 t0 = ktime_get_ns();
		int n;

		p->ui_cfg.settle_full = READ_ONCE(epd_param_settle_full);
		n = fb->format->cpp[0] != 4 ? 0 :
			epd_ui_frame(&p->ui, &p->ui_cfg, vaddr, fb->pitches[0],
				     fb->obj[0]->size - fb->offsets[0], epd_now_ms(), r, &cut);

		p->ui_us_last = div_u64(ktime_get_ns() - t0, 1000);
		p->ui_us_max = max(p->ui_us_max, p->ui_us_last);
		p->ui_frames++;
		if (n) {
			p->ui_updates++;
			p->ui_rects += (u64)n;
			if (cut >= 0) {
				mutex_lock(&p->lock);
				epd_set_cut(p, cut);
				mutex_unlock(&p->lock);
			}
			epd_submit(p, r, n, vaddr, fb->pitches[0], epd_fb_bgr(fb));
			mod_delayed_work(system_unbound_wq, &p->ui_work, 0);
		}
	} else {
		struct drm_atomic_helper_damage_iter iter;
		struct drm_rect r;

		drm_atomic_helper_damage_iter_init(&iter, old, ps);
		drm_atomic_for_each_plane_damage(&iter, &r) {
			struct onyx_epdc_upd u = {
				.top = r.y1, .left = r.x1, .width = drm_rect_width(&r),
				.height = drm_rect_height(&r), .waveform_mode = 0xff,
			};

			epd_request(p, &u, vaddr, fb->pitches[0], epd_fb_bgr(fb));
		}
	}
unlock:
	mutex_unlock(&p->in_lock);
	wake_up(&p->wq);
	drm_dev_exit(idx);
out:
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
}

static const struct drm_plane_helper_funcs epd_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = epd_plane_atomic_check,
	.atomic_update = epd_plane_atomic_update,
};

static void epd_crtc_atomic_flush(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct drm_pending_vblank_event *event = crtc->state->event;

	if (event) {
		crtc->state->event = NULL;
		spin_lock_irq(&crtc->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, event);
		spin_unlock_irq(&crtc->dev->event_lock);
	}
}

static void epd_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct onyx_epd *p = to_epd(crtc->dev);
	struct epd_request r;
	int idx;

	if (!drm_dev_enter(crtc->dev, &idx))
		return;
	mutex_lock(&p->in_lock);
	if (epd_sleep_off(&p->sleep)) {
		epd_ui_gc_request(&r, EPD_W, EPD_H);
		pm_wakeup_ws_event(p->ws, EPD_WAKE_MS, false);
		epd_link_hint(p);
		epd_submit_grey(p, &r, p->sleep_fw->data);
	}
	mutex_unlock(&p->in_lock);
	wake_up(&p->wq);
	drm_dev_exit(idx);
}

static const struct drm_crtc_helper_funcs epd_crtc_helper_funcs = {
	.atomic_check = drm_crtc_helper_atomic_check,
	.atomic_flush = epd_crtc_atomic_flush,
	.atomic_disable = epd_crtc_atomic_disable,
};

static const struct drm_crtc_funcs epd_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_encoder_funcs epd_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static const struct drm_display_mode epd_mode = {
	DRM_MODE_INIT(40, EPD_W, EPD_H, 107, 142),
};

static int epd_connector_get_modes(struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &epd_mode);
}

static const struct drm_connector_helper_funcs epd_connector_helper_funcs = {
	.get_modes = epd_connector_get_modes,
};

static const struct drm_connector_funcs epd_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs epd_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

DEFINE_DRM_GEM_FOPS(epd_fops);

const struct drm_driver epd_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.name = "onyx-epd",
	.desc = "Onyx Boox Go 7 e-ink",
	.major = 1,
	.minor = 0,
	DRM_GEM_SHMEM_DRIVER_OPS,
	.fops = &epd_fops,
};

int epd_modeset_init(struct onyx_epd *p)
{
	struct drm_device *drm = &p->drm;
	int ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.min_width = EPD_W;
	drm->mode_config.max_width = EPD_W;
	drm->mode_config.min_height = EPD_H;
	drm->mode_config.max_height = EPD_H;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &epd_mode_config_funcs;

	ret = drm_universal_plane_init(drm, &p->plane, 0, &epd_plane_funcs, epd_formats,
				       ARRAY_SIZE(epd_formats), NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&p->plane, &epd_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(&p->plane);
	p->updates_prop = drm_property_create(drm, DRM_MODE_PROP_BLOB, "EPD_UPDATES", 0);
	if (!p->updates_prop)
		return -ENOMEM;
	drm_object_attach_property(&p->plane.base, p->updates_prop, 0);

	ret = drm_crtc_init_with_planes(drm, &p->crtc, &p->plane, NULL, &epd_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&p->crtc, &epd_crtc_helper_funcs);

	ret = drm_encoder_init(drm, &p->encoder, &epd_encoder_funcs, DRM_MODE_ENCODER_NONE, NULL);
	if (ret)
		return ret;
	p->encoder.possible_crtcs = drm_crtc_mask(&p->crtc);

	ret = drm_connector_init(drm, &p->connector, &epd_connector_funcs, DRM_MODE_CONNECTOR_DPI);
	if (ret)
		return ret;
	drm_connector_helper_add(&p->connector, &epd_connector_helper_funcs);
	ret = drm_connector_attach_encoder(&p->connector, &p->encoder);
	if (ret)
		return ret;
	drm_mode_config_reset(drm);
	return 0;
}
