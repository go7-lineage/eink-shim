// SPDX-License-Identifier: GPL-2.0-only

#include <linux/dma-buf.h>
#include <linux/dma-direct.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include <drm/drm_drv.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_print.h>

#include "onyx_epd_drv.h"
#include "onyx_epd_uapi.h"

static bool y1_ordered;
module_param(y1_ordered, bool, 0644);
MODULE_PARM_DESC(y1_ordered, "Y1 (flag 0x2000) as a dither fixed to the screen instead of stock's Atkinson per update");

static bool a2_repeat = true;
module_param(a2_repeat, bool, 0644);
MODULE_PARM_DESC(a2_repeat, "finish A2 updates with stock's three further passes");

static bool aa_all;
module_param(aa_all, bool, 0644);
MODULE_PARM_DESC(aa_all, "Regal: pulse every white pixel next to a dark one, as stock, not only next to changing ones");

static bool fast_read = true;
module_param(fast_read, bool, 0644);
MODULE_PARM_DESC(fast_read, "read imported frames through a cached mapping");

static bool epd_upd_to_req(const struct onyx_epdc_upd *u, struct epd_request *r)
{
	if ((u32)u->flag & (7u << 16) ||
	    !epd_request_clip(r, u->left, u->top, u->width, u->height, EPD_W, EPD_H))
		return false;
	r->mode = u->waveform_mode;
	r->update_mode = u->update_mode;
	r->marker = u->update_marker;
	r->flags = u->flag;
	r->once = false;
	r->no_clean = false;
	return true;
}

static void epd_fbmap_drop(struct epd_fbmap *m)
{
	if (!m->obj)
		return;
	vunmap(m->vaddr);
	dma_buf_unmap_attachment_unlocked(m->obj->import_attach, m->sgt, DMA_FROM_DEVICE);
	drm_gem_object_put(m->obj);
	m->obj = NULL;
}

static bool epd_sgt_direct(struct device *dev, struct sg_table *sgt)
{
	struct scatterlist *sg;
	unsigned int i;

	if (sgt->nents != sgt->orig_nents)
		return false;
	for_each_sgtable_dma_sg(sgt, sg, i)
		if (sg_dma_address(sg) != phys_to_dma(dev, sg_phys(sg)) || sg_dma_len(sg) != sg->length)
			return false;
	return true;
}

const u8 *epd_fb_cached(struct onyx_epd *p, struct drm_framebuffer *fb)
{
	struct drm_gem_object *obj = fb->obj[0];
	struct epd_fbmap *m = NULL, *lru = &p->fbmap[0];
	size_t n = obj->size >> PAGE_SHIFT;
	struct dma_buf_attachment *attach;
	int i;

	if (!fast_read || !drm_gem_is_imported(obj))
		return NULL;
	attach = obj->import_attach;
	for (i = 0; i < EPD_NMAP; i++) {
		if (p->fbmap[i].obj == obj)
			m = &p->fbmap[i];
		if (p->fbmap[i].used < lru->used)
			lru = &p->fbmap[i];
	}
	if (!m) {
		struct sg_page_iter it;
		struct sg_table *sgt;
		void *vaddr = NULL;
		size_t k = 0;

		if (n > EPD_MAP_PAGES)
			return NULL;
		sgt = dma_buf_map_attachment_unlocked(attach, DMA_FROM_DEVICE);
		if (IS_ERR(sgt))
			return NULL;
		if (epd_sgt_direct(attach->dev, sgt)) {
			for_each_sgtable_page(sgt, &it, 0) {
				if (k < n)
					p->fbmap_pages[k] = sg_page_iter_page(&it);
				k++;
			}
			if (k == n)
				vaddr = vmap(p->fbmap_pages, n, VM_MAP, PAGE_KERNEL);
		}
		if (!vaddr) {
			dma_buf_unmap_attachment_unlocked(attach, sgt, DMA_FROM_DEVICE);
			return NULL;
		}
		epd_fbmap_drop(lru);
		drm_gem_object_get(obj);
		lru->obj = obj;
		lru->sgt = sgt;
		lru->vaddr = vaddr;
		m = lru;
	}
	m->used = ++p->fbmap_seq;
	dma_sync_sgtable_for_cpu(attach->dev, m->sgt, DMA_FROM_DEVICE);
	return m->vaddr + fb->offsets[0];
}

static void epd_push(struct onyx_epd *p, const struct epd_request *r, int n)
{
	mutex_lock(&p->lock);
	p->pl.pol.a2_once = !READ_ONCE(a2_repeat);
	epd_plane_push(&p->pl, r, n);
	p->kick = true;
	mutex_unlock(&p->lock);
}

void epd_submit(struct onyx_epd *p, const struct epd_request *r, int n, const u8 *rgba, u32 pitch, bool bgr)
{
	int i;

	n = min(n, EPD_UI_MAX_RECTS);
	for (i = 0; i < n; i++)
		epd_convert_update(rgba, pitch, p->pl.in, EPD_W, r[i].left, r[i].top, r[i].width, r[i].height,
				   false, bgr, p->lut);
	epd_push(p, r, n);
}

void epd_submit_grey(struct onyx_epd *p, const struct epd_request *r, const u8 *grey)
{
	epd_convert_grey8(grey, EPD_W, p->pl.in, EPD_W, r->left, r->top, r->width, r->height, p->lut);
	epd_push(p, r, 1);
}

void epd_start(struct onyx_epd *p)
{
	u32 conv = READ_ONCE(y1_ordered) ? EPD_CONV_Y1_ORDERED : 0;
	struct epd_decision d;
	int ret;

	epd_engine_set_aa_all(p->eng, READ_ONCE(aa_all));
	while (epd_plane_pop(&p->pl, p->eng, conv, &d, &ret))
		if (ret)
			drm_dbg(&p->drm, "update mode %d dropped: %d\n", d.upd.mode, ret);
}

void epd_request(struct onyx_epd *p, const struct onyx_epdc_upd *u, const u8 *rgba, u32 pitch, bool bgr)
{
	struct epd_request r;

	if (epd_upd_to_req(u, &r)) {
		epd_link_hint(p);
		epd_submit(p, &r, 1, rgba, pitch, bgr);
	}
}

u64 epd_now_ms(void)
{
	return ktime_to_ms(ktime_get());
}

void epd_set_cut(struct onyx_epd *p, u32 cut)
{
	p->cut = cut;
	if (p->eng)
		epd_engine_set_cut(p->eng, cut);
}

#define EBC_SEND_UPDATE		0x700c

static int epd_resend(struct onyx_epd *p, const struct epd_request *r, int n)
{
	struct iosys_map map[DRM_FORMAT_MAX_PLANES], data[DRM_FORMAT_MAX_PLANES];
	struct drm_plane *plane = &p->plane;
	struct drm_framebuffer *fb = NULL;
	int idx, ret;

	if (!drm_dev_enter(&p->drm, &idx))
		return -ENODEV;
	drm_modeset_lock(&plane->mutex, NULL);
	if (plane->state && plane->state->visible && plane->state->fb) {
		fb = plane->state->fb;
		drm_framebuffer_get(fb);
	}
	drm_modeset_unlock(&plane->mutex);
	ret = -EAGAIN;
	if (!fb)
		goto out;
	ret = drm_gem_fb_vmap(fb, map, data);
	if (ret)
		goto put;
	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (!ret) {
		mutex_lock(&p->in_lock);
		if (!epd_sleep_can_resend(&p->sleep)) {
			ret = -EAGAIN;
		} else if (n) {
			const u8 *fast = epd_fb_cached(p, fb);

			epd_submit(p, r, n, fast ? fast : data[0].vaddr, fb->pitches[0], epd_fb_bgr(fb));
		}
		mutex_unlock(&p->in_lock);
		wake_up(&p->wq);
		drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
	}
	drm_gem_fb_vunmap(fb, map);
put:
	drm_framebuffer_put(fb);
out:
	drm_dev_exit(idx);
	return ret;
}

void epd_redraw(struct onyx_epd *p)
{
	struct epd_request r;

	epd_ui_gc_request(&r, EPD_W, EPD_H);
	pm_wakeup_ws_event(p->ws, EPD_WAKE_MS, false);
	epd_resend(p, &r, 1);
}

static int ebc_open(struct inode *inode, struct file *f)
{
	struct onyx_epd *p = container_of(f->private_data, struct onyx_epd, misc);

	drm_dev_get(&p->drm);
	f->private_data = p;
	return 0;
}

static int ebc_release(struct inode *inode, struct file *f)
{
	struct onyx_epd *p = f->private_data;

	drm_dev_put(&p->drm);
	return 0;
}

static long ebc_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct onyx_epd *p = f->private_data;
	struct onyx_epdc_upd u;
	struct epd_request r;
	int idx, n, ret;

	if (cmd != EBC_SEND_UPDATE)
		return -ENOTTY;
	if (copy_from_user(&u, (void __user *)arg, sizeof(u)))
		return -EFAULT;
	if (!drm_dev_enter(&p->drm, &idx))
		return -ENODEV;
	pm_wakeup_ws_event(p->ws, EPD_WAKE_MS, false);	/* stock: 0x2ee ms */
	n = epd_upd_to_req(&u, &r) ? 1 : 0;
	ret = epd_resend(p, &r, n);
	drm_dev_exit(idx);
	return ret;
}

void epd_ui_work(struct work_struct *work)
{
	struct onyx_epd *p = container_of(to_delayed_work(work), struct onyx_epd, ui_work);
	struct epd_request r[EPD_UI_MAX_RECTS];
	enum epd_ui_step st;
	u64 wait = 0;
	int n;

	mutex_lock(&p->in_lock);
	p->ui_cfg.settle_full = READ_ONCE(epd_param_settle_full);
	st = epd_ui_step(&p->ui, &p->ui_cfg, epd_now_ms(), r, &n, &wait);
	mutex_unlock(&p->in_lock);
	if (st == EPD_UI_WAIT) {
		mod_delayed_work(system_unbound_wq, &p->ui_work, msecs_to_jiffies(wait));
	} else if (st == EPD_UI_SEND && !epd_resend(p, r, n)) {
		p->ui_passes++;
		p->ui_rects += (u64)n;
	}
}

const struct file_operations ebc_fops = {
	.owner = THIS_MODULE,
	.open = ebc_open,
	.release = ebc_release,
	.unlocked_ioctl = ebc_ioctl,
	.compat_ioctl = ebc_ioctl,
	.llseek = noop_llseek,
};

void epd_fbmap_free(void *data)
{
	struct onyx_epd *p = data;
	int i;

	mutex_lock(&p->in_lock);
	for (i = 0; i < EPD_NMAP; i++)
		epd_fbmap_drop(&p->fbmap[i]);
	mutex_unlock(&p->in_lock);
}
