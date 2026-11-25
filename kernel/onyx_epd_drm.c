// SPDX-License-Identifier: GPL-2.0-only
/*
 * Onyx Boox Go 7 e-ink display.
 */

#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/sched/topology.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/vmalloc.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_print.h>

#include "onyx_epd_drv.h"

static char *waveform = "eink_waveform.wbf";
module_param(waveform, charp, 0444);
MODULE_PARM_DESC(waveform, "waveform file");

void epd_stat(struct epd_stat *s, u64 t0)
{
	u32 us = div_u64(ktime_get_ns() - t0, 1000);

	s->last = us;
	s->max = max(s->max, us);
	s->sum += us;
	s->n++;
}

static void epd_thread_place(struct task_struct *t)
{
	unsigned long cap = 0;
	cpumask_var_t big;
	int cpu;

	sched_set_fifo_low(t);
	if (!zalloc_cpumask_var(&big, GFP_KERNEL))
		return;
	for_each_possible_cpu(cpu)
		cap = max(cap, arch_scale_cpu_capacity(cpu));
	for_each_possible_cpu(cpu)
		if (arch_scale_cpu_capacity(cpu) == cap)
			cpumask_set_cpu(cpu, big);
	set_cpus_allowed_ptr(t, big);
	free_cpumask_var(big);
}

static int epd_thread(void *data)
{
	struct onyx_epd *p = data;

	while (!kthread_should_stop()) {
		struct epd_decision d;
		bool busy, was_busy;
		u64 t0;
		int ret;

		wait_event_interruptible(p->wq, kthread_should_stop() || READ_ONCE(p->kick) ||
					 (p->eng && epd_engine_busy(p->eng) && epd_output_ready(p) &&
					  !READ_ONCE(p->suspended)));
		mutex_lock(&p->lock);
		p->kick = false;
		if (p->suspended) {
			mutex_unlock(&p->lock);
			continue;
		}
		was_busy = p->eng && epd_engine_busy(p->eng);
		while (p->eng && epd_queue_pop(&p->q, epd_engine_busy(p->eng), &d)) {
			ret = epd_engine_submit(p->eng, &d.upd, p->grey, EPD_W);
			if (ret)
				drm_dbg(&p->drm, "update mode %d dropped: %d\n", d.upd.mode, ret);
		}
		busy = p->eng && epd_engine_busy(p->eng);
		if (busy && !was_busy && !READ_ONCE(p->t_start))
			WRITE_ONCE(p->t_start, ktime_get_ns());
		if (busy && epd_output_ready(p)) {
			if (!READ_ONCE(p->link_on)) {
				spin_lock(&p->out_lock);
				p->ring.link_req = true;
				spin_unlock(&p->out_lock);
				wake_up(&p->out_wq);
			}
			epd_power_up(p);
			t0 = ktime_get_ns();
			ret = epd_engine_tick(p->eng, p->frame);
			epd_stat(&p->st_tick, t0);
			p->tick_cpu = raw_smp_processor_id();
			busy = epd_engine_busy(p->eng);
			if (!busy && p->q.n)
				p->kick = true;
			mutex_unlock(&p->lock);
			if (ret == 1)
				epd_queue_frame(p);
			mutex_lock(&p->lock);
		}
		if (!busy)
			epd_power_off_later(p);
		mutex_unlock(&p->lock);
	}
	return 0;
}

static void epd_marker_done(void *ctx, u32 marker) { }

static void epd_setup_work(struct work_struct *w)
{
	struct onyx_epd *p = container_of(to_delayed_work(w), struct onyx_epd, setup);
	struct epd_engine_cfg cfg = {
		.timing = &epd_timing_go7,
		.temp_c = READ_ONCE(p->temp_mc) / 1000,
		.tables = { .cut = p->cut, .fill_zero = true, .a2_clean_mode = 2 },
		.marker_done = epd_marker_done,
		.ctx = p,
		.regal_aa = true,
		.supersede = true,
	};
	int ret;

	if (!p->eng) {
		const struct firmware *fw;

		ret = request_firmware_direct(&fw, waveform, p->drm.dev);
		if (!ret) {
			struct epd_engine *eng = epd_engine_create(&cfg, fw->data, fw->size, &ret);

			if (eng) {
				mutex_lock(&p->lock);
				p->fw = fw;
				p->eng = eng;
				epd_engine_set_temp_mc(eng, p->temp_mc);
				mutex_unlock(&p->lock);
				drm_info(&p->drm, "waveform %s loaded\n", waveform);
			} else {
				drm_err(&p->drm, "bad waveform %s: %d\n", waveform, ret);
				release_firmware(fw);
			}
		}
	}
	if (!epd_output_ready(p)) {
		ret = epd_setup_output(p);
		if (ret && ret != -EPROBE_DEFER)
			drm_err(&p->drm, "output setup failed: %d\n", ret);
	}
	if ((!p->eng || !epd_output_ready(p)) && ++p->tries < 240)
		schedule_delayed_work(&p->setup, HZ / 2);
	wake_up(&p->wq);
}

#define EPD_SUSPEND_WAIT_MS	2000

static void epd_hold(struct onyx_epd *p)
{
	ktime_t t0 = ktime_get(), end = ktime_add_ms(t0, EPD_SUSPEND_WAIT_MS);
	u64 frames0 = READ_ONCE(p->frames);
	bool engine, busy;
	int queued;

	for (;;) {
		mutex_lock(&p->lock);
		engine = p->eng && epd_engine_busy(p->eng);
		queued = p->q.n;
		busy = engine || queued;
		if (!busy || !epd_output_ready(p) || ktime_after(ktime_get(), end)) {
			WRITE_ONCE(p->suspended, true);
			mutex_unlock(&p->lock);
			break;
		}
		mutex_unlock(&p->lock);
		usleep_range(10000, 11000);
	}
	if (busy && epd_output_ready(p))
		drm_warn(&p->drm, "suspend: still busy after %lld ms (engine %d, queued %d, frames %llu -> %llu, link %d)\n",
			 ktime_ms_delta(ktime_get(), t0), engine, queued, frames0,
			 READ_ONCE(p->frames), READ_ONCE(p->link_on));
	spin_lock(&p->out_lock);
	p->ring.link_req = false;
	spin_unlock(&p->out_lock);
	cancel_delayed_work_sync(&p->power_off);
	cancel_delayed_work_sync(&p->link_off);
	mutex_lock(&p->lock);
	epd_power_down(p);
	mutex_unlock(&p->lock);
	epd_link_off(p);
}

static void epd_release(struct onyx_epd *p)
{
	mutex_lock(&p->lock);
	WRITE_ONCE(p->suspended, false);
	p->kick = true;
	mutex_unlock(&p->lock);
	wake_up(&p->wq);
}

static int epd_pm_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	struct onyx_epd *p = container_of(nb, struct onyx_epd, pm_nb);

	switch (action) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
	case PM_RESTORE_PREPARE:
		epd_hold(p);
		break;
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		epd_release(p);
		break;
	}
	return NOTIFY_DONE;
}

static int onyx_epd_suspend(struct device *dev)
{
	epd_hold(dev_get_drvdata(dev));
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(onyx_epd_pm, onyx_epd_suspend, NULL);

static void epd_ui_free(void *data)
{
	epd_ui_release(data);
}

static void epd_vfree(void *data)
{
	vfree(data);
}

static void *epd_devm_vzalloc(struct device *dev, size_t size)
{
	void *p = vzalloc(size);

	if (p && devm_add_action_or_reset(dev, epd_vfree, p))
		return NULL;
	return p;
}

static void epd_node_put(void *data)
{
	of_node_put(data);
}

static int epd_link_supplier(struct device *dev, struct device *supplier)
{
	int ret = 0;

	if (!supplier || !supplier->driver)
		ret = -EPROBE_DEFER;
	else if (!device_link_add(dev, supplier, DL_FLAG_AUTOREMOVE_CONSUMER))
		ret = -EINVAL;
	put_device(supplier);
	return ret;
}

static int epd_link_tcon(struct device *dev)
{
	struct device_node *np = of_parse_phandle(dev->of_node, "onyx,tcon", 0);
	struct i2c_client *tcon;

	if (!np)
		return 0;
	tcon = of_find_i2c_device_by_node(np);
	of_node_put(np);
	return epd_link_supplier(dev, tcon ? &tcon->dev : NULL);
}

static int epd_link_output(struct device *dev, struct device_node *dsi_np)
{
	struct device_node *panel_np = of_graph_get_remote_node(dsi_np, 1, 0);
	struct platform_device *host = of_find_device_by_node(dsi_np);
	struct mipi_dsi_device *panel = panel_np ? of_find_mipi_dsi_device_by_node(panel_np) : NULL;
	int ret, ret_panel;

	of_node_put(panel_np);
	ret = epd_link_supplier(dev, host ? &host->dev : NULL);
	ret_panel = epd_link_supplier(dev, panel ? &panel->dev : NULL);
	return ret ?: ret_panel;
}

static int onyx_epd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct epd_timing *t = &epd_timing_go7;
	struct drm_device *out;
	struct onyx_epd *p;
	int ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;
	p = devm_drm_dev_alloc(dev, &epd_drm_driver, struct onyx_epd, drm);
	if (IS_ERR(p))
		return PTR_ERR(p);
	platform_set_drvdata(pdev, p);
	mutex_init(&p->lock);
	mutex_init(&p->in_lock);
	mutex_init(&p->link_lock);
	init_waitqueue_head(&p->wq);
	init_waitqueue_head(&p->out_wq);
	spin_lock_init(&p->out_lock);
	epd_ring_init(&p->ring);
	INIT_DELAYED_WORK(&p->power_off, epd_power_off_work);
	INIT_DELAYED_WORK(&p->link_off, epd_link_off_work);
	INIT_DELAYED_WORK(&p->setup, epd_setup_work);
	INIT_DELAYED_WORK(&p->ui_work, epd_ui_work);
	epd_temp_init(p);
	p->ui_cfg = (struct epd_ui_cfg){ .mode = EPD_UI_SPEED, .gc_interval = 20, .big_pct = 50, .a2cut = 3 };
	p->levels = (struct epd_levels)EPD_LEVELS_IDENTITY;
	p->cut = 3;
	epd_policy_init(&p->pol, true);
	epd_queue_init(&p->q);
	p->q.merge = true;

	p->dsi_np = of_parse_phandle(dev->of_node, "onyx,dsi", 0);
	if (!p->dsi_np)
		return dev_err_probe(dev, -EINVAL, "onyx,dsi\n");
	ret = devm_add_action_or_reset(dev, epd_node_put, p->dsi_np);
	if (ret)
		return ret;
	out = epd_dsi_drm_device(p->dsi_np);
	if (!out || !out->registered)
		return dev_err_probe(dev, -EPROBE_DEFER, "waiting for the DSI host's DRM device\n");
	ret = epd_link_output(dev, p->dsi_np);
	if (ret)
		return dev_err_probe(dev, ret, "DSI host and panel\n");
	ret = epd_link_tcon(dev);
	if (ret)
		return dev_err_probe(dev, ret, "onyx,tcon\n");
	ret = epd_power_init(p);
	if (ret)
		return ret;

	p->grey = epd_devm_vzalloc(dev, EPD_W * EPD_H);
	p->grey_in = epd_devm_vzalloc(dev, EPD_W * EPD_H);
	p->frame = epd_devm_vzalloc(dev, epd_frame_words(t) * 4);
	p->tmpl = epd_devm_vzalloc(dev, epd_frame_words(t) * 4);
	p->err = devm_kcalloc(dev, 3 * EPD_W + 12, sizeof(*p->err), GFP_KERNEL);
	p->fbmap_pages = devm_kcalloc(dev, EPD_MAP_PAGES, sizeof(*p->fbmap_pages), GFP_KERNEL);
	if (!p->grey || !p->grey_in || !p->frame || !p->tmpl || !p->err || !p->fbmap_pages)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, epd_fbmap_free, p);
	if (ret)
		return ret;
	memset(p->grey, 0xff, EPD_W * EPD_H);
	memset(p->grey_in, 0xff, EPD_W * EPD_H);
	epd_frame_template(t, p->frame);
	epd_frame_template(t, p->tmpl);
	p->ui_ok = !epd_ui_init(&p->ui, EPD_W, EPD_H);
	if (p->ui_ok) {
		ret = devm_add_action_or_reset(dev, epd_ui_free, &p->ui);
		if (ret)
			return ret;
	}

	ret = epd_modeset_init(p);
	if (ret)
		return ret;
	p->ws = wakeup_source_register(dev, "epdc_power");
	if (!p->ws)
		return -ENOMEM;
	p->out_thread = kthread_run(epd_out_thread, p, "onyx-epd-out");
	if (IS_ERR(p->out_thread)) {
		ret = PTR_ERR(p->out_thread);
		goto err_ws;
	}
	epd_thread_place(p->out_thread);
	p->thread = kthread_run(epd_thread, p, "onyx-epd");
	if (IS_ERR(p->thread)) {
		ret = PTR_ERR(p->thread);
		kthread_stop(p->out_thread);
		goto err_ws;
	}
	epd_thread_place(p->thread);

	p->misc.minor = MISC_DYNAMIC_MINOR;
	p->misc.name = "ebc";
	p->misc.fops = &ebc_fops;
	p->misc.parent = dev;
	ret = misc_register(&p->misc);
	if (ret)
		goto err_thread;
	p->cls = class_create("sepdc");
	if (IS_ERR(p->cls)) {
		ret = PTR_ERR(p->cls);
		goto err_misc;
	}
	p->sysdev = device_create_with_groups(p->cls, NULL, 0, p, epd_groups, "debug");
	if (IS_ERR(p->sysdev)) {
		ret = PTR_ERR(p->sysdev);
		goto err_cls;
	}
	p->pm_nb.notifier_call = epd_pm_notify;
	ret = register_pm_notifier(&p->pm_nb);
	if (ret)
		goto err_sysdev;
	ret = drm_dev_register(&p->drm, 0);
	if (ret)
		goto err_pm;
	schedule_delayed_work(&p->setup, 0);
	epd_temp_start(p);
	return 0;

err_pm:
	unregister_pm_notifier(&p->pm_nb);
err_sysdev:
	device_unregister(p->sysdev);
err_cls:
	class_destroy(p->cls);
err_misc:
	misc_deregister(&p->misc);
err_thread:
	kthread_stop(p->thread);
	kthread_stop(p->out_thread);
err_ws:
	wakeup_source_unregister(p->ws);
	return ret;
}

static void onyx_epd_remove(struct platform_device *pdev)
{
	struct onyx_epd *p = platform_get_drvdata(pdev);
	int i;

	unregister_pm_notifier(&p->pm_nb);
	device_unregister(p->sysdev);
	class_destroy(p->cls);
	misc_deregister(&p->misc);
	drm_dev_unplug(&p->drm);
	cancel_delayed_work_sync(&p->setup);
	cancel_delayed_work_sync(&p->ui_work);
	epd_temp_stop(p);
	kthread_stop(p->thread);
	kthread_stop(p->out_thread);
	cancel_delayed_work_sync(&p->power_off);
	cancel_delayed_work_sync(&p->link_off);
	epd_power_down(p);
	if (epd_output_ready(p)) {
		for (i = 0; i < EPD_NBUF; i++)
			drm_client_framebuffer_delete(p->buf[i]);
		drm_client_release(&p->client);
	}
	epd_engine_destroy(p->eng);
	release_firmware(p->fw);
	drm_atomic_helper_shutdown(&p->drm);
	wakeup_source_unregister(p->ws);
}

static const struct of_device_id onyx_epd_of_match[] = {
	{ .compatible = "onyx,go7-epd" },
	{ }
};
MODULE_DEVICE_TABLE(of, onyx_epd_of_match);

static struct platform_driver onyx_epd_driver = {
	.probe = onyx_epd_probe,
	.remove = onyx_epd_remove,
	.driver = {
		.name = "onyx-epd",
		.of_match_table = onyx_epd_of_match,
		.dev_groups = epd_groups,
		.pm = pm_sleep_ptr(&onyx_epd_pm),
	},
};
module_platform_driver(onyx_epd_driver);

MODULE_IMPORT_NS("DMA_BUF");
MODULE_DESCRIPTION("Onyx Boox Go 7 e-ink display");
MODULE_LICENSE("GPL");
