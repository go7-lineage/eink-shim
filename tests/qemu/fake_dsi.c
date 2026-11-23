// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_panel.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

struct fake_drm {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
};

struct fake_dsi {
	struct device *dev;
	struct mipi_dsi_host host;
	struct mutex lock;
	struct fake_drm *kms;
	struct drm_bridge *bridge;
	atomic_t frames, enables, disables, dcs, dcs_bad, late;
	bool suspended;
	u32 hash;
	atomic_t driven;
	u8 last_dcs;
};

static struct fake_dsi *to_fake(struct drm_device *drm)
{
	return dev_get_drvdata(drm->dev);
}

static u32 fnv(const u8 *d, size_t n)
{
	u32 h = 2166136261u;

	while (n--) {
		h ^= *d++;
		h *= 16777619u;
	}
	return h;
}

static void fake_enable(struct drm_simple_display_pipe *pipe, struct drm_crtc_state *cs,
			struct drm_plane_state *ps)
{
	struct fake_dsi *f = to_fake(pipe->crtc.dev);

	if (READ_ONCE(f->suspended))
		atomic_inc(&f->late);
	atomic_inc(&f->enables);
}

static void fake_disable(struct drm_simple_display_pipe *pipe)
{
	atomic_inc(&to_fake(pipe->crtc.dev)->disables);
}

static void fake_update(struct drm_simple_display_pipe *pipe, struct drm_plane_state *old)
{
	struct fake_dsi *f = to_fake(pipe->crtc.dev);
	struct drm_plane_state *ps = pipe->plane.state;
	struct drm_shadow_plane_state *s = to_drm_shadow_plane_state(ps);
	struct drm_pending_vblank_event *event = pipe->crtc.state->event;
	struct drm_framebuffer *fb = ps->fb;

	if (fb && ps->visible && s->data[0].vaddr) {
		const u32 *w = s->data[0].vaddr;
		size_t i, n = (size_t)fb->pitches[0] / 4 * fb->height;
		u32 drive = 0;

		for (i = 0; i < n; i++)
			drive += (w[i] & 0xffff) != 0;
		if (drive)
			atomic_inc(&f->driven);
		if (READ_ONCE(f->suspended))
			atomic_inc(&f->late);
		f->hash = fnv(s->data[0].vaddr, (size_t)fb->pitches[0] * fb->height);
		atomic_inc(&f->frames);
	}
	if (event) {
		pipe->crtc.state->event = NULL;
		spin_lock_irq(&pipe->crtc.dev->event_lock);
		drm_crtc_send_vblank_event(&pipe->crtc, event);
		spin_unlock_irq(&pipe->crtc.dev->event_lock);
	}
}

static const struct drm_simple_display_pipe_funcs fake_pipe_funcs = {
	.enable = fake_enable,
	.disable = fake_disable,
	.update = fake_update,
	DRM_GEM_SIMPLE_DISPLAY_PIPE_SHADOW_PLANE_FUNCS,
};

static const struct drm_mode_config_funcs fake_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

DEFINE_DRM_GEM_FOPS(fake_fops);

static const struct drm_driver fake_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.name = "onyx-test-dsi",
	.desc = "fake DSI host for the onyx-epd tests",
	.major = 1,
	DRM_GEM_SHMEM_DRIVER_OPS,
	.fops = &fake_fops,
};

static const u32 fake_formats[] = { DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888 };

static int fake_kms_up(struct fake_dsi *f, struct drm_panel *panel)
{
	struct fake_drm *k;
	int ret;

	k = devm_drm_dev_alloc(f->dev, &fake_driver, struct fake_drm, drm);
	if (IS_ERR(k))
		return PTR_ERR(k);
	ret = drmm_mode_config_init(&k->drm);
	if (ret)
		return ret;
	k->drm.mode_config.max_width = 4096;
	k->drm.mode_config.max_height = 4096;
	k->drm.mode_config.funcs = &fake_mode_config_funcs;
	ret = drm_simple_display_pipe_init(&k->drm, &k->pipe, &fake_pipe_funcs, fake_formats,
					   ARRAY_SIZE(fake_formats), NULL, NULL);
	if (ret)
		return ret;
	f->bridge = drm_panel_bridge_add_typed(panel, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(f->bridge))
		return PTR_ERR(f->bridge);
	ret = drm_simple_display_pipe_attach_bridge(&k->pipe, f->bridge);
	if (ret)
		goto err_bridge;
	drm_mode_config_reset(&k->drm);
	ret = drm_dev_register(&k->drm, 0);
	if (ret)
		goto err_bridge;
	f->kms = k;
	return 0;
err_bridge:
	drm_panel_bridge_remove(f->bridge);
	f->bridge = NULL;
	return ret;
}

static void fake_kms_down(struct fake_dsi *f)
{
	if (!f->kms)
		return;
	drm_dev_unplug(&f->kms->drm);
	drm_atomic_helper_shutdown(&f->kms->drm);
	drm_panel_bridge_remove(f->bridge);
	f->bridge = NULL;
	f->kms = NULL;
}

static int fake_attach(struct mipi_dsi_host *host, struct mipi_dsi_device *dsi)
{
	struct fake_dsi *f = container_of(host, struct fake_dsi, host);
	struct drm_panel *panel = of_drm_find_panel(dsi->dev.of_node);
	int ret;

	if (IS_ERR(panel))
		return PTR_ERR(panel);
	if (dsi->lanes != 2 || dsi->format != MIPI_DSI_FMT_RGB888)
		return -EINVAL;
	mutex_lock(&f->lock);
	ret = f->kms ? -EBUSY : fake_kms_up(f, panel);
	mutex_unlock(&f->lock);
	return ret;
}

static int fake_detach(struct mipi_dsi_host *host, struct mipi_dsi_device *dsi)
{
	struct fake_dsi *f = container_of(host, struct fake_dsi, host);

	mutex_lock(&f->lock);
	fake_kms_down(f);
	mutex_unlock(&f->lock);
	return 0;
}

static ssize_t fake_transfer(struct mipi_dsi_host *host, const struct mipi_dsi_msg *msg)
{
	struct fake_dsi *f = container_of(host, struct fake_dsi, host);
	const u8 *tx = msg->tx_buf;

	if (!msg->tx_len || !(msg->flags & MIPI_DSI_MSG_USE_LPM))
		atomic_inc(&f->dcs_bad);
	if (msg->tx_len)
		f->last_dcs = tx[0];
	atomic_inc(&f->dcs);
	return msg->tx_len;
}

static const struct mipi_dsi_host_ops fake_host_ops = {
	.attach = fake_attach,
	.detach = fake_detach,
	.transfer = fake_transfer,
};

static ssize_t state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct fake_dsi *f = dev_get_drvdata(dev);

	int n;

	n = sysfs_emit(buf, "kms %d frames %d enables %d disables %d ", !!f->kms, atomic_read(&f->frames),
		       atomic_read(&f->enables), atomic_read(&f->disables));
	n += sysfs_emit_at(buf, n, "dcs %d dcs_bad %d last_dcs 0x%02x ", atomic_read(&f->dcs),
			   atomic_read(&f->dcs_bad), f->last_dcs);
	n += sysfs_emit_at(buf, n, "hash %08x driven %d late %d\n", f->hash, atomic_read(&f->driven),
			   atomic_read(&f->late));
	return n;
}
static DEVICE_ATTR_RO(state);

static struct attribute *fake_attrs[] = {
	&dev_attr_state.attr,
	NULL
};
ATTRIBUTE_GROUPS(fake);

static int fake_probe(struct platform_device *pdev)
{
	struct fake_dsi *f;
	int ret;

	f = devm_kzalloc(&pdev->dev, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	f->dev = &pdev->dev;
	ret = devm_mutex_init(&pdev->dev, &f->lock);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, f);
	f->host.dev = &pdev->dev;
	f->host.ops = &fake_host_ops;
	return mipi_dsi_host_register(&f->host);
}

static void fake_remove(struct platform_device *pdev)
{
	struct fake_dsi *f = platform_get_drvdata(pdev);

	mipi_dsi_host_unregister(&f->host);
	mutex_lock(&f->lock);
	fake_kms_down(f);
	mutex_unlock(&f->lock);
}

static int fake_prepare(struct device *dev)
{
	struct fake_dsi *f = dev_get_drvdata(dev);
	int ret = f->kms ? drm_mode_config_helper_suspend(&f->kms->drm) : 0;

	WRITE_ONCE(f->suspended, true);
	return ret;
}

static void fake_complete(struct device *dev)
{
	struct fake_dsi *f = dev_get_drvdata(dev);

	WRITE_ONCE(f->suspended, false);
	if (f->kms)
		drm_mode_config_helper_resume(&f->kms->drm);
}

static const struct dev_pm_ops fake_pm = {
	.prepare = pm_sleep_ptr(fake_prepare),
	.complete = pm_sleep_ptr(fake_complete),
};

static const struct of_device_id fake_of_match[] = {
	{ .compatible = "onyx,test-dsi" },
	{ }
};
MODULE_DEVICE_TABLE(of, fake_of_match);

static struct platform_driver fake_driver_pd = {
	.probe = fake_probe,
	.remove = fake_remove,
	.driver = {
		.name = "onyx-test-dsi",
		.of_match_table = fake_of_match,
		.dev_groups = fake_groups,
		.pm = pm_sleep_ptr(&fake_pm),
	},
};
module_platform_driver(fake_driver_pd);

MODULE_DESCRIPTION("Fake DSI host and display controller for the onyx-epd QEMU tests");
MODULE_LICENSE("GPL");
