// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>

#include <drm/drm_bridge.h>
#include <drm/drm_client.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_print.h>

#include "onyx_epd_drv.h"

static unsigned int link_idle_ms = 2000;
module_param(link_idle_ms, uint, 0644);
MODULE_PARM_DESC(link_idle_ms, "switch DSI link off after this long without frames");

static unsigned int frame_us;
module_param(frame_us, uint, 0644);
MODULE_PARM_DESC(frame_us, "time from one waveform frame to the next, 0 for the waveform's frame rate");

bool epd_param_swap_rg = true;
module_param_named(swap_rg, epd_param_swap_rg, bool, 0644);
MODULE_PARM_DESC(swap_rg, "swap the two data bytes per word");

void epd_link_off(struct onyx_epd *p)
{
	mutex_lock(&p->link_lock);
	if (epd_output_ready(p) && p->link_on) {
		drm_client_modeset_dpms(&p->client, DRM_MODE_DPMS_OFF);
		WRITE_ONCE(p->link_on, false);
	}
	mutex_unlock(&p->link_lock);
}

bool epd_out_idle(struct onyx_epd *p)
{
	bool idle;

	spin_lock(&p->out_lock);
	idle = epd_ring_idle(&p->ring);
	spin_unlock(&p->out_lock);
	return idle;
}

void epd_link_off_work(struct work_struct *w)
{
	struct onyx_epd *p = container_of(to_delayed_work(w), struct onyx_epd, link_off);
	bool idle;

	mutex_lock(&p->lock);
	idle = (!p->eng || !epd_engine_busy(p->eng)) && !p->pl.q.n && epd_out_idle(p);
	mutex_unlock(&p->lock);
	if (idle)
		epd_link_off(p);
	else
		mod_delayed_work(system_wq, &p->link_off, msecs_to_jiffies(link_idle_ms));
}

void epd_link_hint(struct onyx_epd *p)
{
	if (!epd_output_ready(p) || READ_ONCE(p->link_on) || READ_ONCE(p->suspended))
		return;
	spin_lock(&p->out_lock);
	p->ring.link_req = true;
	spin_unlock(&p->out_lock);
	wake_up(&p->out_wq);
}

static bool epd_out_can_queue(struct onyx_epd *p)
{
	bool ok;

	spin_lock(&p->out_lock);
	ok = epd_ring_can_fill(&p->ring);
	spin_unlock(&p->out_lock);
	return ok || kthread_should_stop();
}

static void epd_copy_frame(struct onyx_epd *p, int f, const u32 *src)
{
	u64 t0 = ktime_get_ns();

	epd_frame_copy(&epd_timing_go7, src, p->buf_vaddr[f], p->buf[f]->pitch, epd_param_swap_rg);
	epd_stat(&p->st_copy, t0);
}

void epd_queue_frame(struct onyx_epd *p)
{
	int f;

	wait_event(p->out_wq, epd_out_can_queue(p));
	if (kthread_should_stop())
		return;
	spin_lock(&p->out_lock);
	f = epd_ring_begin_fill(&p->ring);
	spin_unlock(&p->out_lock);
	if (WARN_ON_ONCE(f < 0))
		return;
	epd_copy_frame(p, f, p->frame);
	spin_lock(&p->out_lock);
	epd_ring_end_fill(&p->ring);
	spin_unlock(&p->out_lock);
	wake_up(&p->out_wq);
}

#define EPD_FRAME_MAX_NS	(100 * NSEC_PER_MSEC)

static void epd_pace(struct onyx_epd *p)
{
	u64 period = READ_ONCE(frame_us) ? (u64)READ_ONCE(frame_us) * NSEC_PER_USEC : READ_ONCE(p->frame_ns);
	u64 now = ktime_get_ns(), due = p->t_frame + min_t(u64, period, EPD_FRAME_MAX_NS);

	if (p->t_frame && now < due) {
		unsigned long us = (unsigned long)div_u64(due - now, NSEC_PER_USEC);

		usleep_range(us, us + 50);
		now = ktime_get_ns();
	}
	if (p->t_frame && now - p->t_frame < EPD_FRAME_MAX_NS)
		epd_stat(&p->st_period, p->t_frame);
	p->t_frame = now;
}

static bool epd_out_has_work(struct onyx_epd *p)
{
	bool work;

	spin_lock(&p->out_lock);
	work = epd_ring_has_work(&p->ring);
	spin_unlock(&p->out_lock);
	return work || kthread_should_stop();
}

int epd_out_thread(void *data)
{
	struct onyx_epd *p = data;

	while (!kthread_should_stop()) {
		struct drm_mode_set *ms;
		u64 t0, t_start;
		bool up, null;
		int c;

		wait_event_interruptible(p->out_wq, epd_out_has_work(p));
		spin_lock(&p->out_lock);
		c = epd_ring_take(&p->ring, READ_ONCE(p->link_on), &null);
		spin_unlock(&p->out_lock);
		if (c < 0)
			continue;
		if (READ_ONCE(p->suspended)) {
			spin_lock(&p->out_lock);
			epd_ring_done(&p->ring);
			spin_unlock(&p->out_lock);
			wake_up(&p->out_wq);
			continue;
		}
		if (null) {
			epd_copy_frame(p, c, p->tmpl);
		} else {
			wake_up(&p->out_wq);
			epd_pace(p);
		}

		mutex_lock(&p->client.modeset_mutex);
		drm_client_for_each_modeset(ms, &p->client)
			if (ms->mode)
				ms->fb = p->buf[c]->fb;
		mutex_unlock(&p->client.modeset_mutex);
		mutex_lock(&p->link_lock);
		t0 = ktime_get_ns();
		up = !p->link_on;
		if (drm_client_modeset_commit(&p->client))
			dev_warn_ratelimited(p->drm.dev, "frame commit failed\n");
		else
			WRITE_ONCE(p->link_on, true);
		epd_stat(up ? &p->st_link : &p->st_commit, t0);
		mutex_unlock(&p->link_lock);
		mod_delayed_work(system_wq, &p->link_off, msecs_to_jiffies(link_idle_ms));
		t_start = READ_ONCE(p->t_start);
		if (t_start && !null) {
			epd_stat(&p->st_first, t_start);
			WRITE_ONCE(p->t_start, 0);
		}

		spin_lock(&p->out_lock);
		epd_ring_done(&p->ring);
		spin_unlock(&p->out_lock);
		if (!null)
			p->frames++;
		wake_up(&p->out_wq);
	}
	return 0;
}

struct drm_device *epd_dsi_drm_device(struct device_node *dsi_np)
{
	struct device_node *panel_np = of_graph_get_remote_node(dsi_np, 1, 0);
	struct drm_bridge *bridge;

	if (!panel_np)
		return NULL;
	bridge = of_drm_find_bridge(panel_np);
	of_node_put(panel_np);
	return bridge ? bridge->dev : NULL;
}

int epd_setup_output(struct onyx_epd *p)
{
	struct drm_device *out = epd_dsi_drm_device(p->dsi_np);
	struct iosys_map map;
	int i, ret;

	if (!out || !out->registered)
		return -EPROBE_DEFER;
	ret = drm_client_init(out, &p->client, "onyx-epd", NULL);
	if (ret)
		return ret;
	ret = drm_client_modeset_probe(&p->client, 213, 1699);
	if (ret)
		goto err;
	for (i = 0; i < EPD_NBUF; i++) {
		p->buf[i] = drm_client_framebuffer_create(&p->client, 213, 1699, DRM_FORMAT_ABGR8888);
		if (IS_ERR(p->buf[i])) {
			ret = PTR_ERR(p->buf[i]);
			p->buf[i] = NULL;
			goto err;
		}
		ret = drm_client_buffer_vmap(p->buf[i], &map);
		if (ret)
			goto err;
		p->buf_vaddr[i] = map.vaddr;
	}
	smp_store_release(&p->client_ok, true);
	drm_info(&p->drm, "output on %s, buffer pitch %u\n", out->driver->name, p->buf[0]->pitch);
	epd_link_hint(p);
	return 0;
err:
	for (i = 0; i < EPD_NBUF; i++)
		if (p->buf[i]) {
			drm_client_framebuffer_delete(p->buf[i]);
			p->buf[i] = NULL;
		}
	drm_client_release(&p->client);
	return ret;
}
