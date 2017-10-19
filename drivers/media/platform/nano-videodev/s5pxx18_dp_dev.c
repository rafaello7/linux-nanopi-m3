/*
 * Copyright (C) 2016  Nexell Co., Ltd.
 * Author: junghyun, kim <jhkim@nexell.co.kr>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/module.h>
#include "s5pxx18_dp_dev.h"

#define	LAYER_VIDEO		PLANE_VIDEO_NUM


void nx_soc_dp_plane_video_set_dirty(int module)
{
	nx_mlc_set_dirty_flag(module, LAYER_VIDEO);
}

bool nx_soc_dp_plane_video_is_dirty(int module)
{
	return nx_mlc_get_dirty_flag(module, LAYER_VIDEO);
}

void nx_soc_dp_plane_video_set_format(int module,
			enum nx_mlc_yuvfmt format)
{
	int m_lock_size = 16;

	pr_debug("%s: format=0x%x\n", __func__, format);
	nx_mlc_set_lock_size(module, LAYER_VIDEO, m_lock_size);
	nx_mlc_set_format_yuv(module, format);
}

void nx_soc_dp_plane_video_set_position(int module,
			int src_x, int src_y, int src_w, int src_h,
			int dst_x, int dst_y, int dst_w, int dst_h)
{
	int sx = src_x, sy = src_y;
	int sw = src_w, sh = src_h;
	int dx = dst_x, dy = dst_y;
	int dw = dst_w, dh = dst_h;
	int hf = 1, vf = 1;
	int w = 0, h = 0;

	/*
	 * max scale size 2048
	 * if ove scale size, fix max
	 */
	if (dw > 2048)
		dw = 2048;

	if (dh > 2048)
		dh = 2048;

	w = dx + dw;
	h = dy + dh;

	/* max rectangle 2048 */
	if (w > 2048)
		w = 2048;

	if (h > 2048)
		h = 2048;

	pr_debug("%s: (%d, %d, %d, %d) to (%d, %d, %d, %d, %d, %d)\n",
		 __func__, sx, sy, sw, sh, dx, dy, dw, dh, w, h);

	if (sw == dw && sh == dh)
		hf = 0, vf = 0;

	/* set scale and position */
	nx_mlc_set_video_layer_scale(module, sw, sh, dw, dh, hf, hf, vf, vf);
	nx_mlc_set_position(module, LAYER_VIDEO, dx, dy, w - 1, h - 1);
}

void nx_soc_dp_plane_video_set_address_1p(int module,
		int left, int top,
		unsigned int addr, unsigned int stride)
{
	unsigned int phys = addr + (left/2) + (top * stride);

	pr_debug("%s: lu:0x%x->0x%x,%d\n",
		__func__, addr, phys, stride);

	nx_mlc_set_video_layer_address_yuyv(module, phys, stride);
}

void nx_soc_dp_plane_video_set_address_3p(int module, int left, int top,
		enum nx_mlc_yuvfmt format,
		unsigned int lu_a, unsigned int lu_s,
		unsigned int cb_a, unsigned int cb_s,
		unsigned int cr_a, unsigned int cr_s)
{
	int ls = 1, us = 1;
	int lh = 1, uh = 1;

	switch (format) {
	case nx_mlc_yuvfmt_420:
		us = 2, uh = 2;
		break;
	case nx_mlc_yuvfmt_422:
		us = 2, uh = 1;
		break;
	case nx_mlc_yuvfmt_444:
		us = 1, uh = 1;
		break;
	default:
		return;
	}

	lu_a = lu_a + (left/ls) + (top/lh * lu_s);
	cb_a = cb_a + (left/us) + (top/uh * cb_s);
	cr_a = cr_a + (left/us) + (top/uh * cr_s);

	pr_debug("%s: lu:0x%x,%d, cb:0x%x,%d, cr:0x%x,%d\n",
		__func__, lu_a, lu_s, cb_a, cb_s, cr_a, cr_s);

	nx_mlc_set_video_layer_stride(module, lu_s, cb_s, cr_s);
	nx_mlc_set_video_layer_address(module, lu_a, cb_a, cr_a);
}

void nx_soc_dp_plane_video_set_enable(int module, bool on)
{
	int hl, hc, vl, vc;

	pr_debug("%s: %s\n", __func__, on ? "on" : "off");

	if (on) {
		nx_mlc_set_video_layer_line_buffer_power_mode(module, 1);
		nx_mlc_set_video_layer_line_buffer_sleep_mode(module, 0);
		nx_mlc_set_layer_enable(module, LAYER_VIDEO, 1);
	} else {
		nx_mlc_set_layer_enable(module, LAYER_VIDEO, 0);

		nx_mlc_get_video_layer_scale_filter(module, &hl, &hc, &vl, &vc);
		if (hl | hc | vl | vc)
			nx_mlc_set_video_layer_scale_filter(module, 0, 0, 0, 0);
		nx_mlc_set_video_layer_line_buffer_power_mode(module, 0);
		nx_mlc_set_video_layer_line_buffer_sleep_mode(module, 1);
	}
}

int nx_soc_dp_plane_video_get_priority(int module)
{
	return nx_mlc_get_layer_priority(module);
}

void nx_soc_dp_plane_video_set_priority(int module, int priority)
{
	switch (priority) {
	case 0:
		priority = nx_mlc_priority_videofirst;
		break;	/* PRIORITY-video>0>1>2 */
	case 1:
		priority = nx_mlc_priority_videosecond;
		break;	/* PRIORITY-0>video>1>2 */
	case 2:
		priority = nx_mlc_priority_videothird;
		break;	/* PRIORITY-0>1>video>2 */
	case 3:
		priority = nx_mlc_priority_videofourth;
		break;	/* PRIORITY-0>1>2>video */
	default:
		pr_err(
			"fail : not support video priority num(0~3),(%d)\n",
		    priority);
		return;
	}

	nx_mlc_set_layer_priority(module, priority);
	nx_mlc_set_top_dirty_flag(module);
}
