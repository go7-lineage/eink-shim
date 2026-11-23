// SPDX-License-Identifier: GPL-2.0-only

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/string.h>
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
	bool busy;
	int n, queued;

	mutex_lock(&p->lock);
	busy = p->eng && epd_engine_busy(p->eng);
	queued = p->q.n;
	mutex_unlock(&p->lock);
	spin_lock(&p->out_lock);
	ring = p->ring;
	spin_unlock(&p->out_lock);

	n = sysfs_emit(buf, "waveform %s, output %s, powered %d, frames %llu, swap_rg %d\n",
		       p->eng ? "loaded" : "missing", epd_output_ready(p) ? "ready" : "waiting",
		       p->powered, p->frames, epd_param_swap_rg);
	n += sysfs_emit_at(buf, n, "ui %s: commits %llu, updates %llu, passes %llu, diff %u us (max %u), cached %llu\n",
			   !p->ui_ok ? "unavailable" : epd_param_ui ? epd_ui_mode_name(p->ui_cfg.mode) : "off",
			   p->ui_frames, p->ui_updates, p->ui_passes, p->ui_us_last, p->ui_us_max, p->ui_cached);
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
	return n;
}
static DEVICE_ATTR_RO(status);

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
	p->ui_cfg.mode = mode;
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

static struct attribute *epd_attrs[] = {
	&dev_attr_cut_frame_num.attr,
	&dev_attr_test_pattern.attr,
	&dev_attr_status.attr,
	&dev_attr_ui_mode.attr,
	&dev_attr_ui_gc.attr,
	&dev_attr_ui_full.attr,
	&dev_attr_ui_a2cut.attr,
	NULL
};

static const struct attribute_group epd_group = {
	.attrs = epd_attrs,
};

const struct attribute_group *epd_groups[] = {
	&epd_group,
	NULL
};
