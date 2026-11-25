// SPDX-License-Identifier: GPL-2.0-only

#include <linux/err.h>
#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>

#include "onyx_epd_drv.h"

#define EPD_TEMP_PERIOD	(60 * HZ)

static int temp_c = 24;
module_param(temp_c, int, 0644);
MODULE_PARM_DESC(temp_c, "panel temperature in degrees C while temp_zone can't be read");

static char *temp_zone = "qcom_qg";
module_param(temp_zone, charp, 0444);
MODULE_PARM_DESC(temp_zone, "thermal zone that stands in for the panel temperature, empty for temp_c");

static int epd_temp_read(int *temp_mc)
{
	struct thermal_zone_device *tz;

	if (!*temp_zone)
		return -ENOENT;
	tz = thermal_zone_get_zone_by_name(temp_zone);
	if (IS_ERR(tz))
		return PTR_ERR(tz);
	return thermal_zone_get_temp(tz, temp_mc);
}

static void epd_temp_work(struct work_struct *w)
{
	struct onyx_epd *p = container_of(to_delayed_work(w), struct onyx_epd, temp_work);
	int temp_mc, ret = epd_temp_read(&temp_mc);

	if (ret)
		temp_mc = READ_ONCE(temp_c) * 1000;
	mutex_lock(&p->lock);
	p->temp_mc = temp_mc;
	p->temp_err = ret;
	if (p->eng)
		epd_engine_set_temp_mc(p->eng, temp_mc);
	mutex_unlock(&p->lock);
	queue_delayed_work(system_freezable_wq, &p->temp_work, EPD_TEMP_PERIOD);
}

void epd_temp_init(struct onyx_epd *p)
{
	INIT_DEFERRABLE_WORK(&p->temp_work, epd_temp_work);
	p->temp_mc = temp_c * 1000;
	p->temp_err = -EAGAIN;
}

void epd_temp_start(struct onyx_epd *p)
{
	queue_delayed_work(system_freezable_wq, &p->temp_work, 0);
}

void epd_temp_stop(struct onyx_epd *p)
{
	cancel_delayed_work_sync(&p->temp_work);
}

int epd_temp_emit(struct onyx_epd *p, char *buf, int at)
{
	int temp_mc, err, lo = 0, hi = 0, n;
	bool eng;

	mutex_lock(&p->lock);
	temp_mc = p->temp_mc;
	err = p->temp_err;
	eng = p->eng;
	if (p->eng)
		epd_engine_temp_range(p->eng, &lo, &hi);
	mutex_unlock(&p->lock);

	n = sysfs_emit_at(buf, at, "temperature %d mC from %s", temp_mc, err ? "temp_c" : temp_zone);
	if (err && *temp_zone)
		n += sysfs_emit_at(buf, at + n, " (%s: %d)", temp_zone, err);
	if (eng)
		n += sysfs_emit_at(buf, at + n, ", waveform range %d-%d C", lo, hi);
	n += sysfs_emit_at(buf, at + n, "\n");
	return n;
}
