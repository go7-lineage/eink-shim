// SPDX-License-Identifier: GPL-2.0-only

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/string_choices.h>
#include <linux/vmalloc.h>

#include "onyx_epd_drv.h"
#include "onyx_epd_uapi.h"
#include "core/epd_pattern.h"

static ssize_t cut_frame_num_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct onyx_epd *p = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", p->cut);
}

static ssize_t cut_frame_num_store(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct onyx_epd *p = dev_get_drvdata(dev);
	unsigned int v;
	int ret = kstrtouint(buf, 0, &v);

	if (ret)
		return ret;
	if (v > 255)
		return -EINVAL;
	mutex_lock(&p->lock);
	epd_set_cut(p, v);
	mutex_unlock(&p->lock);
	return count;
}
static DEVICE_ATTR_RW(cut_frame_num);

static int epd_emit_stat(char *buf, int at, const char *name, const struct epd_stat *s, const char *end)
{
	return sysfs_emit_at(buf, at, " %s %u/%u/%u%s", name, s->last,
			     (u32)(s->n ? div64_u64(s->sum, s->n) : 0), s->max, end);
}

static ssize_t status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct onyx_epd *p = dev_get_drvdata(dev);
	struct epd_ring ring;
	bool busy, pmic, pmic_night, pmic_rb_ok, poweroff;
	u8 pmic_rb[EPD_PMIC_NREGS];
	struct epd_sleep sl;
	int n, queued;
	u32 flashes, flash_drops;

	mutex_lock(&p->lock);
	busy = p->eng && epd_engine_busy(p->eng);
	queued = p->q.n;
	pmic = !!p->pmic;
	pmic_night = p->pmic_night;
	pmic_rb_ok = p->pmic_rb_ok;
	memcpy(pmic_rb, p->pmic_rb, sizeof(pmic_rb));
	mutex_unlock(&p->lock);
	spin_lock(&p->out_lock);
	ring = p->ring;
	spin_unlock(&p->out_lock);
	mutex_lock(&p->in_lock);
	flashes = p->ui.flashes;
	flash_drops = p->ui.flash_drops;
	mutex_unlock(&p->in_lock);

	n = sysfs_emit(buf, "waveform %s, output %s, powered %d, frames %llu, swap_rg %d\n",
		       p->eng ? "loaded" : "missing", epd_output_ready(p) ? "ready" : "waiting",
		       p->powered, p->frames, epd_param_swap_rg);
	n += epd_temp_emit(p, buf, n);
	n += sysfs_emit_at(buf, n,
			   "ui %s: commits %llu, updates %llu, passes %llu, rects %llu, diff %u us (max %u), cached %llu",
			   !p->ui_ok ? "unavailable" : epd_param_ui ? epd_ui_mode_name(p->ui_cfg.mode) : "off",
			   p->ui_frames, p->ui_updates, p->ui_passes, p->ui_rects, p->ui_us_last, p->ui_us_max,
			   p->ui_cached);
	n += sysfs_emit_at(buf, n, ", flashes %u (dropped %u)\n", flashes, flash_drops);
	n += sysfs_emit_at(buf, n, "us last/avg/max (cpu %d):", p->tick_cpu);
	n += epd_emit_stat(buf, n, "tick", &p->st_tick, ",");
	n += epd_emit_stat(buf, n, "copy", &p->st_copy, ",");
	n += epd_emit_stat(buf, n, "commit", &p->st_commit, "\n ");
	n += epd_emit_stat(buf, n, "link up", &p->st_link, ",");
	n += epd_emit_stat(buf, n, "power up", &p->st_power, ",");
	n += epd_emit_stat(buf, n, "first frame", &p->st_first, "\n");
	n += sysfs_emit_at(buf, n, "engine busy %d, queued %d, link %d, ", busy, queued, READ_ONCE(p->link_on));
	n += sysfs_emit_at(buf, n, "buffers shown %d inflight %d pending %d filling %d, link request %d\n",
			   ring.shown, ring.inflight, ring.pending, ring.filling, ring.link_req);
	mutex_lock(&p->in_lock);
	sl = p->sleep;
	poweroff = !!p->poweroff_fw;
	mutex_unlock(&p->in_lock);
	n += sysfs_emit_at(buf, n, "sleep image %s, shown %llu, wake refreshes %llu, modesets skipped %llu\n",
			   sl.image ? "loaded" : "missing", sl.n_shown, sl.n_full, sl.n_skipped);
	n += sysfs_emit_at(buf, n, "power-off image %s, shutdown %d\n", poweroff ? "loaded" : "missing",
			   sl.shutdown);
	n += sysfs_emit_at(buf, n, "pmic night: %s", pmic ? str_on_off(pmic_night) : "unavailable");
	if (pmic && pmic_rb_ok)
		n += sysfs_emit_at(buf, n, ", chip reads %*ph", EPD_PMIC_NREGS, pmic_rb);
	else if (pmic)
		n += sysfs_emit_at(buf, n, ", chip reads -");
	n += sysfs_emit_at(buf, n, "\n");
	return n;
}
static DEVICE_ATTR_RO(status);

static bool epd_lut_refresh(struct onyx_epd *p)
{
	u8 lut[256];
	bool active = epd_lut_build(lut, p->gamma_on ? &p->levels : NULL, epd_ui_mode_inverted(p->ui_cfg.mode));
	bool changed = active != !!p->lut || (active && memcmp(lut, p->lut_buf, sizeof(lut)));

	if (changed && active)
		memcpy(p->lut_buf, lut, sizeof(lut));
	p->lut = active ? p->lut_buf : NULL;
	return changed;
}

static ssize_t ui_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct onyx_epd *p = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", epd_ui_mode_name(p->ui_cfg.mode));
}

static ssize_t ui_mode_store(struct device *dev, struct device_attribute *attr, const char *buf,
			     size_t count)
{
	struct onyx_epd *p = dev_get_drvdata(dev);
	char name[16];
	int mode;

	strscpy(name, buf, sizeof(name));
	mode = epd_ui_mode_by_name(strim(name));
	if (mode < 0)
		return mode;
	mutex_lock(&p->in_lock);
	WRITE_ONCE(p->ui_cfg.mode, mode);
	epd_lut_refresh(p);
	mutex_unlock(&p->in_lock);
	return count;
}
static DEVICE_ATTR_RW(ui_mode);

#define EPD_UI_INT_ATTR(name, field)								\
static ssize_t name##_show(struct device *dev, struct device_attribute *attr, char *buf)	\
{												\
	struct onyx_epd *p = dev_get_drvdata(dev);						\
												\
	return sysfs_emit(buf, "%d\n", p->ui_cfg.field);					\
}												\
static ssize_t name##_store(struct device *dev, struct device_attribute *attr,			\
			    const char *buf, size_t count)					\
{												\
	struct onyx_epd *p = dev_get_drvdata(dev);						\
	int v, ret = kstrtoint(buf, 0, &v);							\
												\
	if (ret)										\
		return ret;									\
	mutex_lock(&p->in_lock);								\
	p->ui_cfg.field = v;									\
	mutex_unlock(&p->in_lock);								\
	return count;										\
}												\
static DEVICE_ATTR_RW(name)

EPD_UI_INT_ATTR(ui_gc, gc_interval);
EPD_UI_INT_ATTR(ui_full, big_pct);
EPD_UI_INT_ATTR(ui_a2cut, a2cut);

static ssize_t epd_gamma_set(struct onyx_epd *p, const bool *on, const struct epd_levels *l, size_t count)
{
	bool changed;

	mutex_lock(&p->in_lock);
	if (on)
		p->gamma_on = *on;
	if (l)
		p->levels = *l;
	changed = epd_lut_refresh(p);
	mutex_unlock(&p->in_lock);
	if (changed)
		epd_redraw(p);
	return count;
}

static ssize_t ui_gamma_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct onyx_epd *p = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", p->gamma_on);
}

static ssize_t ui_gamma_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	bool on;
	int ret = kstrtobool(buf, &on);

	if (ret)
		return ret;
	return epd_gamma_set(dev_get_drvdata(dev), &on, NULL, count);
}
static DEVICE_ATTR_RW(ui_gamma);

static ssize_t ui_levels_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct onyx_epd *p = dev_get_drvdata(dev);
	struct epd_levels l;

	mutex_lock(&p->in_lock);
	l = p->levels;
	mutex_unlock(&p->in_lock);
	return sysfs_emit(buf, "%u %u %u\n", l.black, l.gamma, l.white);
}

static ssize_t ui_levels_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct epd_levels l;
	int ret = epd_levels_parse(buf, &l);

	if (ret)
		return ret;
	return epd_gamma_set(dev_get_drvdata(dev), NULL, &l, count);
}
static DEVICE_ATTR_RW(ui_levels);

static ssize_t test_pattern_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct onyx_epd *p = dev_get_drvdata(dev);
	struct onyx_epdc_upd u = { .width = EPD_W, .height = EPD_H, .waveform_mode = 2, .update_mode = 1 };
	char name[16];
	int mode, n, pattern;
	u32 *img;

	n = sscanf(buf, "%15s %d", name, &mode);
	if (n < 1)
		return -EINVAL;
	if (n == 2)
		u.waveform_mode = mode;
	pattern = epd_pattern_by_name(name);
	if (pattern < 0)
		return pattern;
	img = vmalloc(EPD_W * EPD_H * 4);
	if (!img)
		return -ENOMEM;
	epd_pattern_fill(pattern, img, EPD_W, EPD_H);
	mutex_lock(&p->in_lock);
	epd_request(p, &u, (const u8 *)img, EPD_W * 4, false);
	mutex_unlock(&p->in_lock);
	wake_up(&p->wq);
	vfree(img);
	return count;
}
static DEVICE_ATTR_WO(test_pattern);

static ssize_t ui_flash_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct onyx_epd *p = dev_get_drvdata(dev);
	unsigned int v;
	int ret = kstrtouint(buf, 0, &v);

	if (ret)
		return ret;
	if (v != 1)
		return -EINVAL;
	if (!p->ui_ok)
		return -ENODEV;
	mutex_lock(&p->in_lock);
	epd_ui_flash(&p->ui, epd_now_ms());
	mutex_unlock(&p->in_lock);
	mod_delayed_work(system_unbound_wq, &p->ui_work, 0);
	return count;
}
static DEVICE_ATTR_WO(ui_flash);

static ssize_t shutdown_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct onyx_epd *p = dev_get_drvdata(dev);
	bool v;
	int ret = kstrtobool(buf, &v);

	if (ret)
		return ret;
	mutex_lock(&p->in_lock);
	p->sleep.shutdown = v;
	mutex_unlock(&p->in_lock);
	return count;
}
static DEVICE_ATTR_WO(shutdown);

static struct attribute *epd_attrs[] = {
	&dev_attr_cut_frame_num.attr,
	&dev_attr_test_pattern.attr,
	&dev_attr_status.attr,
	&dev_attr_ui_mode.attr,
	&dev_attr_ui_gc.attr,
	&dev_attr_ui_full.attr,
	&dev_attr_ui_a2cut.attr,
	&dev_attr_ui_gamma.attr,
	&dev_attr_ui_levels.attr,
	&dev_attr_ui_flash.attr,
	&dev_attr_shutdown.attr,
	NULL
};

static const struct attribute_group epd_group = {
	.attrs = epd_attrs,
};

const struct attribute_group *epd_groups[] = {
	&epd_group,
	NULL
};
