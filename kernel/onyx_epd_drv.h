/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ONYX_EPD_DRV_H
#define ONYX_EPD_DRV_H

#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <drm/drm_client.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_plane.h>

#include "core/epd_convert.h"
#include "core/epd_engine.h"
#include "core/epd_gamma.h"
#include "core/epd_policy.h"
#include "core/epd_ring.h"
#include "core/epd_sleep.h"
#include "core/epd_ui.h"

struct onyx_epdc_upd;

#define EPD_W		1264
#define EPD_H		1680
#define EPD_NBUF	EPD_RING_BUFS

#define EPD_NMAP	4
#define EPD_MAP_PAGES	4096

struct epd_fbmap {
	struct drm_gem_object *obj;
	struct sg_table *sgt;
	void *vaddr;
	u64 used;
};

enum { EPD_V3P3, EPD_DISPLAY, EPD_VCOM, EPD_NRAILS };

struct onyx_epd {
	struct drm_device drm;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct drm_property *updates_prop;

	struct mutex lock;
	struct mutex in_lock;
	struct epd_engine *eng;
	const struct firmware *fw;
	struct epd_policy pol;
	struct epd_queue q;
	u8 *grey;
	u8 *grey_in;
	int *err;
	struct epd_fbmap fbmap[EPD_NMAP];
	u64 fbmap_seq;
	struct page **fbmap_pages;
	u32 cut;

	struct epd_ui ui;
	bool ui_ok;
	struct epd_ui_cfg ui_cfg;
	struct epd_levels levels;
	bool gamma_on;
	const u8 *gamma;
	u8 gamma_lut[256];
	struct delayed_work ui_work;
	u64 ui_frames, ui_updates, ui_passes, ui_rects, ui_cached;
	u32 ui_us_last, ui_us_max;
	struct epd_sleep sleep;

	struct epd_stat { u32 last, max; u64 sum, n; } st_tick, st_copy, st_commit, st_link, st_power, st_first;
	u64 t_start;
	int tick_cpu;

	struct device_node *dsi_np;
	struct drm_client_dev client;
	bool client_ok;
	struct mutex link_lock;
	bool link_on;
	struct drm_client_buffer *buf[EPD_NBUF];
	void *buf_vaddr[EPD_NBUF];
	u32 *frame;

	struct task_struct *out_thread;
	wait_queue_head_t out_wq;
	spinlock_t out_lock;
	struct epd_ring ring;
	u32 *tmpl;

	struct task_struct *thread;
	wait_queue_head_t wq;
	bool kick;
	bool suspended;
	struct notifier_block pm_nb;

	struct regulator *rail[EPD_NRAILS];
	bool rail_on[EPD_NRAILS];
	bool powered;
	struct delayed_work power_off;
	struct delayed_work link_off;
	struct delayed_work setup;
	int tries;
	struct delayed_work temp_work;
	int temp_mc;
	int temp_err;
	u64 frames;

	struct miscdevice misc;
	struct class *cls;
	struct device *sysdev;
	struct wakeup_source *ws;
};

static inline struct onyx_epd *to_epd(struct drm_device *drm)
{
	return container_of(drm, struct onyx_epd, drm);
}

static inline bool epd_output_ready(struct onyx_epd *p)
{
	return smp_load_acquire(&p->client_ok);
}

extern bool epd_param_ui;
extern bool epd_param_swap_rg;
extern const struct drm_driver epd_drm_driver;
extern const struct file_operations ebc_fops;
extern const struct attribute_group *epd_groups[];

void epd_stat(struct epd_stat *s, u64 t0);

int epd_power_init(struct onyx_epd *p);
void epd_power_up(struct onyx_epd *p);
void epd_power_down(struct onyx_epd *p);
void epd_power_off_later(struct onyx_epd *p);
void epd_power_off_work(struct work_struct *w);

void epd_temp_init(struct onyx_epd *p);
void epd_temp_start(struct onyx_epd *p);
void epd_temp_stop(struct onyx_epd *p);
int epd_temp_emit(struct onyx_epd *p, char *buf, int at);

struct drm_device *epd_dsi_drm_device(struct device_node *dsi_np);
int epd_setup_output(struct onyx_epd *p);
void epd_link_off(struct onyx_epd *p);
void epd_link_off_work(struct work_struct *w);
void epd_link_hint(struct onyx_epd *p);
bool epd_out_idle(struct onyx_epd *p);
void epd_queue_frame(struct onyx_epd *p);
int epd_out_thread(void *data);

const u8 *epd_fb_cached(struct onyx_epd *p, struct drm_framebuffer *fb);
void epd_fbmap_free(void *data);
void epd_submit(struct onyx_epd *p, const struct epd_request *r, int n, const u8 *rgba, u32 pitch, bool bgr);
void epd_request(struct onyx_epd *p, const struct onyx_epdc_upd *u, const u8 *rgba, u32 pitch, bool bgr);
void epd_redraw(struct onyx_epd *p);
void epd_ui_work(struct work_struct *work);
u64 epd_now_ms(void);
void epd_set_cut(struct onyx_epd *p, u32 cut);

bool epd_fb_bgr(const struct drm_framebuffer *fb);
int epd_modeset_init(struct onyx_epd *p);

#endif
