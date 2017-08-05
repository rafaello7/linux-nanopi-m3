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
#ifndef _S5PXX18_DP_HDMI_H_
#define _S5PXX18_DP_HDMI_H_

#include <drm/drmP.h>
#include <video/videomode.h>

#include "s5pxx18_drm_dp.h"

enum {
	RES_FIELD_INTERLACED	= 0x1,
	RES_FIELD_NVSYNC		= 0x2,
	RES_FIELD_NHSYNC		= 0x4
};

struct hdmi_res_mode {
	int pixelclock;
	int h_as, h_sw, h_bp, h_fp;
	int v_as, v_sw, v_bp, v_fp;
	u16 refresh;
	unsigned long flags;
	char *name;
};

enum color_range {
	AVI_FULL_RANGE = 0,
	AVI_LIMITED_RANGE
};

struct hdmi_preset {
	struct hdmi_res_mode mode;
	enum hdmi_picture_aspect aspect_ratio;
	bool dvi_mode;
	u8 vic;
};

enum hdmi_vformat {
	HDMI_VIDEO_FORMAT_2D = 0x0,
	HDMI_VIDEO_FORMAT_3D = 0x2
};

enum hdmi_3d_type {
	HDMI_3D_TYPE_FP = 0x0, /** Frame Packing */
	HDMI_3D_TYPE_TB = 0x6, /** Top-and-Bottom */
	HDMI_3D_TYPE_SB_HALF = 0x8 /** Side-by-Side Half */
};

struct hdmi_format {
	enum hdmi_vformat vformat;
	enum hdmi_3d_type type_3d;
};

struct hdmi_conf {
	const struct hdmi_preset *preset;
	const struct hdmi_format *format;
	const u8 *phy_data;
};

/* VENDOR header */
#define HDMI_VSI_VERSION		0x01
#define HDMI_VSI_LENGTH			0x05

/* AVI header */
#define HDMI_AVI_VERSION		0x02
#define HDMI_AVI_LENGTH			0x0d
#define AVI_UNDERSCAN			(2 << 0)
#define AVI_ACTIVE_FORMAT_VALID	(1 << 4)
#define AVI_SAME_AS_PIC_ASPECT_RATIO 0x8
#define AVI_ITU709				(2 << 6)
#define AVI_LIMITED_RANGE		(1 << 2)
#define AVI_FULL_RANGE			(2 << 2)

/* AUI header info */
#define HDMI_AUI_VERSION		0x01
#define HDMI_AUI_LENGTH			0x0a

enum HDMI_3D_EXT_DATA {
	/* refer to Table H-3 3D_Ext_Data - Additional video format
	 * information for Side-by-side(half) 3D structure */

	/** Horizontal sub-sampleing */
	HDMI_H_SUB_SAMPLE = 0x1
};

enum HDMI_OUTPUT_FMT {
	HDMI_OUTPUT_RGB888 = 0x0,
	HDMI_OUTPUT_YUV444 = 0x2
};

enum HDMI_AUDIO_CODEC {
	HDMI_AUDIO_PCM,
	HDMI_AUDIO_AC3,
	HDMI_AUDIO_MP3
};

/* HPD events */
#define	HDMI_EVENT_PLUG		(1<<0)
#define	HDMI_EVENT_UNPLUG	(1<<1)
#define	HDMI_EVENT_HDCP		(1<<2)

extern const struct hdmi_conf hdmi_conf[];
extern const int num_hdmi_presets;

int nx_dp_device_hdmi_register(struct device *dev,
			struct device_node *np, struct dp_control_dev *dpc);

u32  nx_dp_hdmi_hpd_event(int irq);
bool nx_dp_hdmi_is_connected(void);
bool nx_dp_hdmi_mode_valid(const struct drm_display_mode*);
int nx_dp_hdmi_mode_set(struct nx_drm_device *display,
			struct drm_display_mode *mode,
			bool dvi_mode, int q_range);
int  nx_dp_hdmi_mode_commit(struct nx_drm_device *display, int crtc);
void nx_dp_hdmi_power(struct nx_drm_device *display, bool on);
int nx_dp_hdmi_resume(struct nx_drm_device *display);
int nx_dp_hdmi_suspend(struct nx_drm_device *display);

#endif
