// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/of.h>
#include <linux/device.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_print.h>

#include "onyx_epd_drv.h"
#include "core/epd_bootargs.h"

static bool hv = true;
module_param(hv, bool, 0644);
MODULE_PARM_DESC(hv, "switch the panel rails on for updates");

static unsigned int idle_ms = 500;
module_param(idle_ms, uint, 0644);

static const char *const epd_rail_names[EPD_NRAILS] = { "v3p3", "display", "vcom" };

void epd_power_up(struct onyx_epd *p)
{
	u64 t0 = ktime_get_ns();
	int i;

	cancel_delayed_work(&p->power_off);
	if (p->powered || !hv)
		return;
	__pm_stay_awake(p->ws);
	for (i = 0; i < EPD_NRAILS; i++) {
		if (!p->rail[i])
			continue;
		p->rail_on[i] = !regulator_enable(p->rail[i]);
		if (!p->rail_on[i])
			drm_warn(&p->drm, "%s on failed\n", epd_rail_names[i]);
	}
	p->powered = true;
	epd_stat(&p->st_power, t0);
}

void epd_power_down(struct onyx_epd *p)
{
	int i;

	if (!p->powered)
		return;
	for (i = EPD_NRAILS - 1; i >= 0; i--) {
		if (p->rail_on[i])
			regulator_disable(p->rail[i]);
		p->rail_on[i] = false;
	}
	p->powered = false;
	__pm_relax(p->ws);
}

void epd_power_off_later(struct onyx_epd *p)
{
	if (p->powered)
		schedule_delayed_work(&p->power_off, msecs_to_jiffies(idle_ms));
}

void epd_power_off_work(struct work_struct *w)
{
	struct onyx_epd *p = container_of(to_delayed_work(w), struct onyx_epd, power_off);
	bool idle;

	mutex_lock(&p->lock);
	idle = (!p->eng || !epd_engine_busy(p->eng)) && epd_out_idle(p);
	if (idle)
		epd_power_down(p);
	else if (!p->eng || !epd_engine_busy(p->eng))	/* last frames still going out */
		schedule_delayed_work(&p->power_off, msecs_to_jiffies(idle_ms));
	mutex_unlock(&p->lock);
}

static void epd_set_vcom(struct onyx_epd *p)
{
	struct regulator *vcom = p->rail[EPD_VCOM];
	const char *args;
	int ret, step;
	u32 uv;

	if (!vcom || !of_chosen || of_property_read_string(of_chosen, "bootargs", &args))
		return;
	ret = epd_bootargs_vcom(args, &uv);
	if (ret == -ENOENT)
		return;
	if (ret) {
		drm_warn(&p->drm, "bad VCOM in the bootloader's arguments: %d\n", ret);
		return;
	}
	step = max_t(int, regulator_get_linear_step(vcom), 1);
	if (regulator_set_voltage(vcom, (int)uv - step + 1, (int)uv))
		drm_warn(&p->drm, "can't set VCOM %u uV\n", uv);
	else
		drm_info(&p->drm, "VCOM %u uV from the bootloader\n", uv);
}

static struct regulator *epd_supply(struct device *dev, const char *name)
{
	struct regulator *r = devm_regulator_get_optional(dev, name);

	if (IS_ERR(r) && PTR_ERR(r) == -ENODEV)
		return NULL;
	return r;
}

int epd_power_init(struct onyx_epd *p)
{
	struct device *dev = p->drm.dev;
	int i;

	for (i = 0; i < EPD_NRAILS; i++) {
		p->rail[i] = epd_supply(dev, epd_rail_names[i]);
		if (IS_ERR(p->rail[i]))
			return dev_err_probe(dev, PTR_ERR(p->rail[i]), "%s-supply\n", epd_rail_names[i]);
	}
	epd_set_vcom(p);
	return 0;
}
