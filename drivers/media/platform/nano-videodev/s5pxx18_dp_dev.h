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
#ifndef _S5PXX18_DP_DEV_H_
#define _S5PXX18_DP_DEV_H_

#include "s5pxx18_soc_mlc.h"

#define	PLANE_VIDEO_NUM			(3) /* Planes = 0,1 (RGB), 3 (VIDEO) */

/* for prototype layer index */
enum dp_color_type {
	dp_color_colorkey,
	dp_color_alpha,
	dp_color_bright,
	dp_color_hue,
	dp_color_contrast,
	dp_color_saturation,
	dp_color_gamma,
	dp_color_transp,
	dp_color_invert,
};

void nx_soc_dp_plane_video_set_format(int module,
			enum nx_mlc_yuvfmt format);
void nx_soc_dp_plane_video_set_position(int module,
			int src_x, int src_y, int src_w, int src_h,
			int dst_x, int dst_y, int dst_w, int dst_h);
void nx_soc_dp_plane_video_set_address_1p(int module, int left, int top,
		unsigned int addr, unsigned int stride);
void nx_soc_dp_plane_video_set_address_3p(int module, int left, int top,
		enum nx_mlc_yuvfmt format,
		unsigned int lu_a, unsigned int lu_s,
		unsigned int cb_a, unsigned int cb_s,
		unsigned int cr_a, unsigned int cr_s);
void nx_soc_dp_plane_video_set_enable(int module, bool on);
int nx_soc_dp_plane_video_get_priority(int module);
void nx_soc_dp_plane_video_set_priority(int module, int priority);
bool nx_soc_dp_plane_video_is_dirty(int module);
void nx_soc_dp_plane_video_set_dirty(int module);

#endif /* __S5PXX18_DP_DEV_H__ */
