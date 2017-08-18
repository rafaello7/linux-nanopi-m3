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
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/reset.h>
#include <linux/hdmi.h>

#include "s5pxx18_dp_hdmi.h"


/*
 * HDMI preset configs
 */

/* CEAVideoModes */
static const struct hdmi_preset hdmi_conf_640x480p60 = {
	.mode = {
		.pixelclock = 25175000,
		.h_as = 640, .h_sw = 96, .h_bp = 48, .h_fp = 16,
		.v_as = 480, .v_sw = 2, .v_bp = 33, .v_fp = 10,
		.refresh = 60,		/* 59.9405 */
		.name = "vic1,640x480@60Hz 4:3",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_4_3,
	.vic = 1
};

static const struct hdmi_preset hdmi_conf_720x480p60_4a3 = {
	.mode = {
		.pixelclock = 27000000,
		.h_as = 720, .h_sw = 62, .h_bp = 60, .h_fp = 16,
		.v_as = 480, .v_sw = 6, .v_bp = 30, .v_fp = 9,
		.refresh = 60,		/* 59.9401 */
		.name = "vic2,720x480@60Hz 4:3",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_4_3,
	.vic = 2
};

static const struct hdmi_preset hdmi_conf_720x480p60_16a9 = {
	.mode = {
		.pixelclock = 27000000,
		.h_as = 720, .h_sw = 62, .h_bp = 60, .h_fp = 16,
		.v_as = 480, .v_sw = 6, .v_bp = 30, .v_fp = 9,
		.refresh = 60,		/* 59.9401 */
		.name = "vic3,720x480@60Hz 16:9",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 3
};

static const struct hdmi_preset hdmi_conf_1280x720p60 = {
	.mode = {
		.pixelclock = 74250000,
		.h_as = 1280, .h_sw = 40, .h_bp = 220, .h_fp = 110,
		.v_as = 720, .v_sw = 5, .v_bp = 20, .v_fp = 5,
		.refresh = 60,
		.name = "vic4,1280x720@60Hz 16:9",
		.flags = 0
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 4
};

static const struct hdmi_preset hdmi_conf_1920x1080i60 = {
	.mode = {
		.pixelclock = 74250000,
		.h_as = 1920, .h_sw = 44, .h_bp = 148, .h_fp = 88,
		.v_as = 1080, .v_sw = 10, .v_bp = 31, .v_fp = 4,
		.refresh = 60,
		.name = "vic5,1920x1080i@60Hz 16:9",
		.flags = RES_FIELD_INTERLACED
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 5
};

static const struct hdmi_preset hdmi_conf_1440x480i60_4a3 = {
	.mode = {
		.pixelclock = 27000000,
		.h_as = 1440, .h_sw = 124, .h_bp = 114, .h_fp = 38,
		.v_as = 480, .v_sw = 6, .v_bp = 31, .v_fp = 8,
		.refresh = 60,		/* 59.9401 */
		.name = "vic6,1440x480i@60Hz 4:3",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC | RES_FIELD_INTERLACED
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_4_3,
	.vic = 6
};

static const struct hdmi_preset hdmi_conf_1440x480i60_16a9 = {
	.mode = {
		.pixelclock = 27000000,
		.h_as = 1440, .h_sw = 124, .h_bp = 114, .h_fp = 38,
		.v_as = 480, .v_sw = 6, .v_bp = 31, .v_fp = 8,
		.refresh = 60,		/* 59.9401 */
		.name = "vic7,1440x480i@60Hz 16:9",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC | RES_FIELD_INTERLACED
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 7
};

static const struct hdmi_preset hdmi_conf_1440x240p60_4a3 = {
	.mode = {
		.pixelclock = 27000000,
		.h_as = 1440, .h_sw = 124, .h_bp = 114, .h_fp = 38,
		.v_as = 240, .v_sw = 3, .v_bp = 15, .v_fp = 4,
		.refresh = 60,		/* 60.0544 */
		.name = "vic8,1440x240@60Hz 4:3",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_4_3,
	.vic = 8
};

static const struct hdmi_preset hdmi_conf_1440x240p60_16a9 = {
	.mode = {
		.pixelclock = 27000000,
		.h_as = 1440, .h_sw = 124, .h_bp = 114, .h_fp = 38,
		.v_as = 240, .v_sw = 3, .v_bp = 15, .v_fp = 4,
		.refresh = 60,		/* 60.0544 */
		.name = "vic9,1440x240@60Hz 16:9",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 9
};

static const struct hdmi_preset hdmi_conf_1440x480p60_4a3 = {
	.mode = {
		.pixelclock = 54000000,
		.h_as = 1440, .h_sw = 124, .h_bp = 120, .h_fp = 32,
		.v_as = 480, .v_sw = 6, .v_bp = 30, .v_fp = 9,
		.refresh = 60,		/* 59.9401 */
		.name = "vic14,1440x480@60Hz 4:3",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_4_3,
	.vic = 14
};

static const struct hdmi_preset hdmi_conf_1440x480p60_16a9 = {
	.mode = {
		.pixelclock = 54000000,
		.h_as = 1440, .h_sw = 124, .h_bp = 120, .h_fp = 32,
		.v_as = 480, .v_sw = 6, .v_bp = 30, .v_fp = 9,
		.refresh = 60,		/* 59.9401 */
		.name = "vic15,1440x480@60Hz 16:9",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 15
};

static const struct hdmi_preset hdmi_conf_1920x1080p60 = {
	.mode = {
		.pixelclock = 148500000,
		.h_as = 1920, .h_sw = 44, .h_bp = 148, .h_fp = 88,
		.v_as = 1080, .v_sw = 5, .v_bp = 36, .v_fp = 4,
		.refresh = 60,
		.name = "vic16,1920x1080@60Hz 16:9",
		.flags = 0
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 16
};

static const struct hdmi_preset hdmi_conf_720x576p50_4a3 = {
	.mode = {
		.pixelclock = 27000000,
		.h_as = 720, .h_sw = 64, .h_bp = 68, .h_fp = 12,
		.v_as = 576, .v_sw = 5, .v_bp = 39, .v_fp = 5,
		.refresh = 50,
		.name = "vic17,720x576@50Hz 4:3",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_4_3,
	.vic = 17
};

static const struct hdmi_preset hdmi_conf_720x576p50_16a9 = {
	.mode = {
		.pixelclock = 27000000,
		.h_as = 720, .h_sw = 64, .h_bp = 68, .h_fp = 12,
		.v_as = 576, .v_sw = 5, .v_bp = 39, .v_fp = 5,
		.refresh = 50,
		.name = "vic18,720x576@50Hz 16:9",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 18
};

static const struct hdmi_preset hdmi_conf_1280x720p50 = {
	.mode = {
		.pixelclock = 74250000,
		.h_as = 1280, .h_sw = 40, .h_bp = 220, .h_fp = 440,
		.v_as = 720, .v_sw = 5, .v_bp = 20, .v_fp = 5,
		.refresh = 50,
		.name = "vic19,1280x720@50Hz 16:9",
		.flags = 0
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 19
};

static const struct hdmi_preset hdmi_conf_1920x1080i50 = {
	.mode = {
		.pixelclock = 74250000,
		.h_as = 1920, .h_sw = 44, .h_bp = 148, .h_fp = 528,
		.v_as = 1080, .v_sw = 10, .v_bp = 31, .v_fp = 4,
		.refresh = 50,
		.name = "vic20,1920x1080i@50Hz 16:9",
		.flags = RES_FIELD_INTERLACED
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 20
};

static const struct hdmi_preset hdmi_conf_1440x576i50_4a3 = {
	.mode = {
		.pixelclock = 27000000,
		.h_as = 1440, .h_sw = 126, .h_bp = 138, .h_fp = 24,
		.v_as = 576, .v_sw = 6, .v_bp = 39, .v_fp = 4,
		.refresh = 50,
		.name = "vic21,1440x576i@50Hz 4:3",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC | RES_FIELD_INTERLACED
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_4_3,
	.vic = 21
};

static const struct hdmi_preset hdmi_conf_1440x576i50_16a9 = {
	.mode = {
		.pixelclock = 27000000,
		.h_as = 1440, .h_sw = 126, .h_bp = 138, .h_fp = 24,
		.v_as = 576, .v_sw = 6, .v_bp = 39, .v_fp = 4,
		.refresh = 50,
		.name = "vic22,1440x576i@50Hz 4:3",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC | RES_FIELD_INTERLACED
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 22
};

static const struct hdmi_preset hdmi_conf_1440x288p50_4a3 = {
	.mode = {
		.pixelclock = 27000000,
		.h_as = 1440, .h_sw = 126, .h_bp = 138, .h_fp = 24,
		.v_as = 288, .v_sw = 3, .v_bp = 19, .v_fp = 2,
		.refresh = 50,		/* 50.0801 */
		.name = "vic23,1440x288@50Hz 4:3",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_4_3,
	.vic = 23
};

static const struct hdmi_preset hdmi_conf_1440x288p50_16a9 = {
	.mode = {
		.pixelclock = 27000000,
		.h_as = 1440, .h_sw = 126, .h_bp = 138, .h_fp = 24,
		.v_as = 288, .v_sw = 3, .v_bp = 19, .v_fp = 2,
		.refresh = 50,		/* 50.0801 */
		.name = "vic24,1440x288@50Hz 16:9",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 24
};

static const struct hdmi_preset hdmi_conf_1440x576p50_4a3 = {
	.mode = {
		.pixelclock = 54000000,
		.h_as = 1440, .h_sw = 128, .h_bp = 136, .h_fp = 24,
		.v_as = 576, .v_sw = 5, .v_bp = 39, .v_fp = 5,
		.refresh = 50,
		.name = "vic29,1440x576@50Hz 4:3",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_4_3,
	.vic = 29
};

static const struct hdmi_preset hdmi_conf_1440x576p50_16a9 = {
	.mode = {
		.pixelclock = 54000000,
		.h_as = 1440, .h_sw = 128, .h_bp = 136, .h_fp = 24,
		.v_as = 576, .v_sw = 5, .v_bp = 39, .v_fp = 5,
		.refresh = 50,
		.name = "vic30,1440x576@50Hz 16:9",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 30
};

static const struct hdmi_preset hdmi_conf_1920x1080p50 = {
	.mode = {
		.pixelclock = 148500000,
		.h_as = 1920, .h_sw = 44, .h_bp = 148, .h_fp = 528,
		.v_as = 1080, .v_sw = 5, .v_bp = 36, .v_fp = 4,
		.refresh = 50,
		.name = "vic31,1920x1080@50Hz 16:9",
		.flags = 0
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 31
};

static const struct hdmi_preset hdmi_conf_1920x1080p24 = {
	.mode = {
		.pixelclock = 74250000,
		.h_as = 1920, .h_sw = 44, .h_bp = 148, .h_fp = 638,
		.v_as = 1080, .v_sw = 5, .v_bp = 36, .v_fp = 4,
		.refresh = 24,
		.name = "vic32,1920x1080@24Hz 16:9",
		.flags = 0
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 32
};

static const struct hdmi_preset hdmi_conf_1920x1080p25 = {
	.mode = {
		.pixelclock = 74250000,
		.h_as = 1920, .h_sw = 44, .h_bp = 148, .h_fp = 528,
		.v_as = 1080, .v_sw = 5, .v_bp = 36, .v_fp = 4,
		.refresh = 25,
		.name = "vic33,1920x1080@25Hz 16:9",
		.flags = 0
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 33
};

static const struct hdmi_preset hdmi_conf_1920x1080p30 = {
	.mode = {
		.pixelclock = 74250000,
		.h_as = 1920, .h_sw = 44, .h_bp = 148, .h_fp = 88,
		.v_as = 1080, .v_sw = 5, .v_bp = 36, .v_fp = 4,
		.refresh = 30,
		.name = "vic34,1920x1080@30Hz 16:9",
		.flags = 0
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 34
};

static const struct hdmi_preset hdmi_conf_1920x1080i100 = {
	.mode = {
		.pixelclock = 148500000,
		.h_as = 1920, .h_sw = 44, .h_bp = 148, .h_fp = 528,
		.v_as = 1080, .v_sw = 10, .v_bp = 31, .v_fp = 4,
		.refresh = 100,
		.name = "vic40,1920x1080i@100Hz 16:9",
		.flags = RES_FIELD_INTERLACED
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 40
};

static const struct hdmi_preset hdmi_conf_1280x720p100 = {
	.mode = {
		.pixelclock = 148500000,
		.h_as = 1280, .h_sw = 40, .h_bp = 220, .h_fp = 440,
		.v_as = 720, .v_sw = 5, .v_bp = 20, .v_fp = 5,
		.refresh = 100,
		.name = "vic41,1280x720@100Hz 16:9",
		.flags = 0
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 41
};

static const struct hdmi_preset hdmi_conf_720x576p100_4a3 = {
	.mode = {
		.pixelclock = 54000000,
		.h_as = 720, .h_sw = 64, .h_bp = 68, .h_fp = 12,
		.v_as = 576, .v_sw = 5, .v_bp = 39, .v_fp = 5,
		.refresh = 100,
		.name = "vic42,720x576@100Hz 4:3",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_4_3,
	.vic = 42
};

static const struct hdmi_preset hdmi_conf_720x576p100_16a9 = {
	.mode = {
		.pixelclock = 54000000,
		.h_as = 720, .h_sw = 64, .h_bp = 68, .h_fp = 12,
		.v_as = 576, .v_sw = 5, .v_bp = 39, .v_fp = 5,
		.refresh = 100,
		.name = "vic43,720x576@100Hz 16:9",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 43
};

static const struct hdmi_preset hdmi_conf_1440x576i100_4a3 = {
	.mode = {
		.pixelclock = 54000000,
		.h_as = 1440, .h_sw = 126, .h_bp = 138, .h_fp = 24,
		.v_as = 576, .v_sw = 6, .v_bp = 39, .v_fp = 4,
		.refresh = 100,		/* 50 */
		.name = "vic44,1440x576i@100Hz 4:3",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC | RES_FIELD_INTERLACED
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_4_3,
	.vic = 44
};

static const struct hdmi_preset hdmi_conf_1440x576i100_16a9 = {
	.mode = {
		.pixelclock = 54000000,
		.h_as = 1440, .h_sw = 126, .h_bp = 138, .h_fp = 24,
		.v_as = 576, .v_sw = 6, .v_bp = 39, .v_fp = 4,
		.refresh = 100,		/* 50 */
		.name = "vic45,1440x576i@100Hz 16:9",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC | RES_FIELD_INTERLACED
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 45
};

static const struct hdmi_preset hdmi_conf_1920x1080i120 = {
	.mode = {
		.pixelclock = 148500000,
		.h_as = 1920, .h_sw = 44, .h_bp = 148, .h_fp = 88,
		.v_as = 1080, .v_sw = 10, .v_bp = 31, .v_fp = 4,
		.refresh = 120,
		.name = "vic46,1920x1080i@120Hz 16:9",
		.flags = RES_FIELD_INTERLACED
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 46
};

static const struct hdmi_preset hdmi_conf_1280x720p120 = {
	.mode = {
		.pixelclock = 148500000,
		.h_as = 1280, .h_sw = 40, .h_bp = 220, .h_fp = 110,
		.v_as = 720, .v_sw = 5, .v_bp = 20, .v_fp = 5,
		.refresh = 120,
		.name = "vic47,1280x720@120Hz 16:9",
		.flags = 0
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 47
};

static const struct hdmi_preset hdmi_conf_720x480p120_4a3 = {
	.mode = {
		.pixelclock = 54000000,
		.h_as = 720, .h_sw = 62, .h_bp = 60, .h_fp = 16,
		.v_as = 480, .v_sw = 6, .v_bp = 30, .v_fp = 9,
		.refresh = 120,		/* 119.88 */
		.name = "vic48,720x480@120Hz 4:3",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_4_3,
	.vic = 48
};

static const struct hdmi_preset hdmi_conf_720x480p120_16a9 = {
	.mode = {
		.pixelclock = 54000000,
		.h_as = 720, .h_sw = 62, .h_bp = 60, .h_fp = 16,
		.v_as = 480, .v_sw = 6, .v_bp = 30, .v_fp = 9,
		.refresh = 120,		/* 119.88 */
		.name = "vic49,720x480@120Hz 16:9",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 49
};

static const struct hdmi_preset hdmi_conf_1440x480i120_4a3 = {
	.mode = {
		.pixelclock = 54000000,
		.h_as = 1440, .h_sw = 124, .h_bp = 114, .h_fp = 38,
		.v_as = 480, .v_sw = 6, .v_bp = 31, .v_fp = 8,
		.refresh = 120,		/* 119.88 */
		.name = "vic50,1440x480i@120Hz 4:3",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC | RES_FIELD_INTERLACED
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_4_3,
	.vic = 50
};

static const struct hdmi_preset hdmi_conf_1440x480i120_16a9 = {
	.mode = {
		.pixelclock = 54000000,
		.h_as = 1440, .h_sw = 124, .h_bp = 114, .h_fp = 38,
		.v_as = 480, .v_sw = 6, .v_bp = 31, .v_fp = 8,
		.refresh = 120,		/* 119.88 */
		.name = "vic51,1440x480i@120Hz 16:9",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC | RES_FIELD_INTERLACED
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 51
};

static const struct hdmi_preset hdmi_conf_1280x720p25 = {
	.mode = {
		.pixelclock = 74250000,
		.h_as = 1280, .h_sw = 40, .h_bp = 220, .h_fp = 2420,
		.v_as = 720, .v_sw = 5, .v_bp = 20, .v_fp = 5,
		.refresh = 25,
		.name = "vic61,1280x720@25Hz 16:9",
		.flags = 0
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 61
};

static const struct hdmi_preset hdmi_conf_1280x720p30 = {
	.mode = {
		.pixelclock = 74250000,
		.h_as = 1280, .h_sw = 40, .h_bp = 220, .h_fp = 1760,
		.v_as = 720, .v_sw = 5, .v_bp = 20, .v_fp = 5,
		.refresh = 30,
		.name = "vic62,1280x720@30Hz 16:9",
		.flags = 0
	},
	.aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
	.vic = 62
};


/* DDCEstablishedModes */

static const struct hdmi_preset hdmi_conf_1024x768p70 = {
	.mode = {
		.pixelclock = 75000000,
		.h_as = 1024, .h_sw = 136, .h_bp = 144, .h_fp = 24,
		.v_as = 768, .v_sw = 6, .v_bp = 29, .v_fp = 3,
		.refresh = 70,		/* 70.0694 */
		.name = "1024x768@70Hz",
		.flags = RES_FIELD_NHSYNC | RES_FIELD_NVSYNC
	}
};

/* DMTModes */

static const struct hdmi_preset hdmi_conf_800x600p85 = {
	.mode = {
		.pixelclock = 56250000,
		.h_as = 800, .h_sw = 64, .h_bp = 152, .h_fp = 32,
		.v_as = 600, .v_sw = 3, .v_bp = 27, .v_fp = 1,
		.refresh = 85,		/* 85.0613 */
		.name = "800x600@85Hz",
		.flags = 0
	}
};

static const struct hdmi_preset hdmi_conf_1280x768p60 = {
	.mode = {
		.pixelclock = 79500000,
		.h_as = 1280, .h_sw = 128, .h_bp = 192, .h_fp = 64,
		.v_as = 768, .v_sw = 7, .v_bp = 20, .v_fp = 3,
		.refresh = 60,		/* 59.8702 */
		.name = "1280x768@60Hz",
		.flags = RES_FIELD_NHSYNC
	}
};

static const struct hdmi_preset hdmi_conf_1280x800p120 = {
	.mode = {
		.pixelclock = 146250000,
		.h_as = 1280, .h_sw = 32, .h_bp = 80, .h_fp = 48,
		.v_as = 800, .v_sw = 6, .v_bp = 38, .v_fp = 3,
		.refresh = 120,		/* 119.909 */
		.name = "1280x800@120Hz RB",
		.flags = RES_FIELD_NVSYNC
	}
};

static const struct hdmi_preset hdmi_conf_1280x960p85 = {
	.mode = {
		.pixelclock = 148500000,
		.h_as = 1280, .h_sw = 160, .h_bp = 224, .h_fp = 64,
		.v_as = 960, .v_sw = 3, .v_bp = 47, .v_fp = 1,
		.refresh = 85,		/* 85.0025 */
		.name = "1280x960@85Hz",
		.flags = 0
	}
};

static const struct hdmi_preset hdmi_conf_1280x1024p85 = {
	.mode = {
		.pixelclock = 157500000,
		.h_as = 1280, .h_sw = 160, .h_bp = 224, .h_fp = 64,
		.v_as = 1024, .v_sw = 3, .v_bp = 44, .v_fp = 1,
		.refresh = 85,		/* 85.0241 */
		.name = "1280x1024@85Hz",
		.flags = 0
	}
};

static const struct hdmi_preset hdmi_conf_1360x768p120 = {
	.mode = {
		.pixelclock = 148250000,
		.h_as = 1360, .h_sw = 32, .h_bp = 80, .h_fp = 48,
		.v_as = 768, .v_sw = 5, .v_bp = 37, .v_fp = 3,
		.refresh = 120,		/* 119.967 */
		.name = "1360x768@120Hz RB",
		.flags = RES_FIELD_NVSYNC
	}
};

static const struct hdmi_preset hdmi_conf_1400x1050p75 = {
	.mode = {
		.pixelclock = 156000000,
		.h_as = 1400, .h_sw = 144, .h_bp = 248, .h_fp = 104,
		.v_as = 1050, .v_sw = 4, .v_bp = 42, .v_fp = 3,
		.refresh = 75,		/* 74.8667 */
		.name = "1400x1050@75Hz",
		.flags = RES_FIELD_NHSYNC
	}
};

static const struct hdmi_preset hdmi_conf_1440x900p85 = {
	.mode = {
		.pixelclock = 157000000,
		.h_as = 1440, .h_sw = 152, .h_bp = 256, .h_fp = 104,
		.v_as = 900, .v_sw = 6, .v_bp = 39, .v_fp = 3,
		.refresh = 85,		/* 84.8421 */
		.name = "1440x900@85Hz",
		.flags = RES_FIELD_NHSYNC
	}
};

static const struct hdmi_preset hdmi_conf_1680x1050p60 = {
	.mode = {
		.pixelclock = 146250000,
		.h_as = 1680, .h_sw = 176, .h_bp = 280, .h_fp = 104,
		.v_as = 1050, .v_sw = 6, .v_bp = 30, .v_fp = 3,
		.refresh = 60,		/* 59.9543 */
		.name = "1680x1050@60Hz",
		.flags = RES_FIELD_NHSYNC
	}
};

static const struct hdmi_preset hdmi_conf_1920x1200p60 = {
	.mode = {
		.pixelclock = 154000000,
		.h_as = 1920, .h_sw = 32, .h_bp = 80, .h_fp = 48,
		.v_as = 1200, .v_sw = 6, .v_bp = 26, .v_fp = 3,
		.refresh = 60,		/* 59.9502 */
		.name = "1920x1200@60Hz RB",
		.flags = RES_FIELD_NVSYNC
	}
};

/*
 * PHY preset data tables
 */
static const u8 hdmiphy_preset_25_2[32] = {
	0x52, 0x3f, 0x55, 0x40, 0x01, 0x00, 0xc8, 0x82,
	0xc8, 0xbd, 0xd8, 0x45, 0xa0, 0xac, 0x80, 0x0a,
	0x80, 0x01, 0x84, 0x05, 0x22, 0x24, 0x86, 0x54,
	0xf4, 0x24, 0x00, 0x00, 0x00, 0x01, 0x80, 0x10,
};

static const u8 hdmiphy_preset_25_175[32] = {
	0xd1, 0x1f, 0x50, 0x40, 0x20, 0x1e, 0xc8, 0x81,
	0xe8, 0xbd, 0xd8, 0x45, 0xa0, 0xac, 0x80, 0x0a,
	0x80, 0x09, 0x84, 0x05, 0x22, 0x24, 0x86, 0x54,
	0xf4, 0x24, 0x00, 0x00, 0x00, 0x01, 0x80, 0x10,
};

static const u8 hdmiphy_preset_27[32] = {
	0xd1, 0x22, 0x51, 0x40, 0x08, 0xfc, 0xe0, 0x98,
	0xe8, 0xcb, 0xd8, 0x45, 0xa0, 0xac, 0x80, 0x0a,
	0x80, 0x09, 0x84, 0x05, 0x22, 0x24, 0x86, 0x54,
	0xe4, 0x24, 0x00, 0x00, 0x00, 0x01, 0x80, 0x10,
};

static const u8 hdmiphy_preset_27_027[32] = {
	0xd1, 0x2d, 0x72, 0x40, 0x64, 0x12, 0xc8, 0x43,
	0xe8, 0x0e, 0xd9, 0x45, 0xa0, 0xac, 0x80, 0x0a,
	0x80, 0x09, 0x84, 0x05, 0x22, 0x24, 0x86, 0x54,
	0xe3, 0x24, 0x00, 0x00, 0x00, 0x01, 0x80, 0x10,
};

static const u8 hdmiphy_preset_54[32] = {
	0x54, 0x2d, 0x35, 0x40, 0x01, 0x00, 0xc8, 0x82,
	0xc8, 0x0e, 0xd9, 0x45, 0xa0, 0xac, 0x80, 0x0a,
	0x80, 0x09, 0x84, 0x05, 0x22, 0x24, 0x86, 0x54,
	0xe4, 0x24, 0x01, 0x00, 0x00, 0x01, 0x80, 0x10,
};

static const u8 hdmiphy_preset_54_054[32] = {
	0xd1, 0x2d, 0x32, 0x40, 0x64, 0x12, 0xc8, 0x43,
	0xe8, 0x0e, 0xd9, 0x45, 0xa0, 0xac, 0x80, 0x0a,
	0x80, 0x09, 0x84, 0x05, 0x22, 0x24, 0x86, 0x54,
	0xe3, 0x24, 0x01, 0x00, 0x00, 0x01, 0x80, 0x10,
};

static const u8 hdmiphy_preset_74_175[32] = {
	0xd1, 0x1f, 0x10, 0x40, 0x5b, 0xef, 0xc8, 0x81,
	0xe8, 0xb9, 0xd8, 0x45, 0xa0, 0xac, 0x80, 0x0a,
	0x80, 0x09, 0x84, 0x05, 0x22, 0x24, 0x86, 0x54,
	0xa6, 0x24, 0x01, 0x00, 0x00, 0x01, 0x80, 0x10,
};

static const u8 hdmiphy_preset_74_25[32] = {
	0xd1, 0x1f, 0x10, 0x40, 0x40, 0xf8, 0xc8, 0x81,
	0xe8, 0xba, 0xd8, 0x45, 0xa0, 0xac, 0x80, 0x08,
	0x80, 0x09, 0x84, 0x05, 0x22, 0x24, 0x86, 0x54,
	0xa5, 0x24, 0x01, 0x00, 0x00, 0x01, 0x80, 0x10,
};

static const u8 hdmiphy_preset_148_352[32] = {
	0xd1, 0x1f, 0x00, 0x40, 0x5b, 0xef, 0xc8, 0x81,
	0xe8, 0xb9, 0xd8, 0x45, 0xa0, 0xac, 0x80, 0x0a,
	0x80, 0x09, 0x84, 0x05, 0x22, 0x24, 0x86, 0x54,
	0x4b, 0x25, 0x03, 0x00, 0x00, 0x01, 0x80, 0x10,
};

static const u8 hdmiphy_preset_148_5[32] = {
	0xd1, 0x1f, 0x00, 0x40, 0x40, 0xf8, 0xc8, 0x81,
	0xe8, 0xba, 0xd8, 0x45, 0xa0, 0xac, 0x80, 0x08,
	0x80, 0x09, 0x84, 0x05, 0x22, 0x24, 0x86, 0x54,
	0x4b, 0x25, 0x03, 0x00, 0x00, 0x01, 0x80, 0x10,
};

static const struct hdmi_format _format_2d = {
	.vformat = HDMI_VIDEO_FORMAT_2D,
};

const struct hdmi_conf hdmi_conf[] = {
	/* CEAVideoModes */
	{
		.preset = &hdmi_conf_640x480p60,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_25_175
	}, {
		.preset = &hdmi_conf_720x480p60_4a3,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_27
	}, {
		.preset = &hdmi_conf_720x480p60_16a9,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_27
	}, {
		.preset = &hdmi_conf_1280x720p60,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_74_25
	}, {
		.preset = &hdmi_conf_1920x1080i60,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_74_25
	}, {
		.preset = &hdmi_conf_1440x480i60_4a3,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_27
	}, {
		.preset = &hdmi_conf_1440x480i60_16a9,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_27
	}, {
		.preset = &hdmi_conf_1440x240p60_4a3,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_27
	}, {
		.preset = &hdmi_conf_1440x240p60_16a9,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_27
	}, {
		.preset = &hdmi_conf_1440x480p60_4a3,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_54
	}, {
		.preset = &hdmi_conf_1440x480p60_16a9,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_54
	}, {
		.preset = &hdmi_conf_1920x1080p60,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_148_5
	}, {
		.preset = &hdmi_conf_720x576p50_4a3,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_27
	}, {
		.preset = &hdmi_conf_720x576p50_16a9,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_27
	}, {
		.preset = &hdmi_conf_1280x720p50,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_74_25
	}, {
		.preset = &hdmi_conf_1920x1080i50,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_74_25
	}, {
		.preset = &hdmi_conf_1440x576i50_4a3,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_27
	}, {
		.preset = &hdmi_conf_1440x576i50_16a9,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_27
	}, {
		.preset = &hdmi_conf_1440x288p50_4a3,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_27
	}, {
		.preset = &hdmi_conf_1440x288p50_16a9,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_27
	}, {
		.preset = &hdmi_conf_1440x576p50_4a3,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_54
	}, {
		.preset = &hdmi_conf_1440x576p50_16a9,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_54
	}, {
		.preset = &hdmi_conf_1920x1080p50,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_148_5
	}, {
		.preset = &hdmi_conf_1920x1080p24,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_74_25
	}, {
		.preset = &hdmi_conf_1920x1080p25,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_74_25
	}, {
		.preset = &hdmi_conf_1920x1080p30,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_74_25
	}, {
		.preset = &hdmi_conf_1920x1080i100,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_148_5
	}, {
		.preset = &hdmi_conf_1280x720p100,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_148_5
	}, {
		.preset = &hdmi_conf_720x576p100_4a3,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_54
	}, {
		.preset = &hdmi_conf_720x576p100_16a9,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_54
	}, {
		.preset = &hdmi_conf_1440x576i100_4a3,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_54
	}, {
		.preset = &hdmi_conf_1440x576i100_16a9,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_54
	}, {
		.preset = &hdmi_conf_1920x1080i120,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_148_5
	}, {
		.preset = &hdmi_conf_1280x720p120,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_148_5
	}, {
		.preset = &hdmi_conf_720x480p120_4a3,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_54
	}, {
		.preset = &hdmi_conf_720x480p120_16a9,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_54
	}, {
		.preset = &hdmi_conf_1440x480i120_4a3,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_54
	}, {
		.preset = &hdmi_conf_1440x480i120_16a9,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_54
	}, {
		.preset = &hdmi_conf_1280x720p25,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_74_25
	}, {
		.preset = &hdmi_conf_1280x720p30,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_74_25
	},
	/* DDCEstablishedModes */
	{
		.preset = &hdmi_conf_1024x768p70,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_74_25		// hdmiphy_preset_75
	},
	/* DMTModes */
	{
		.preset = &hdmi_conf_800x600p85,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_54_054		// hdmiphy_preset_56_25
	}, {
		.preset = &hdmi_conf_1280x768p60,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_74_25		// hdmiphy_preset_79_5
	}, {
		.preset = &hdmi_conf_1280x800p120,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_148_352		// hdmiphy_preset_146_25
	}, {
		.preset = &hdmi_conf_1280x960p85,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_148_5
	}, {
		.preset = &hdmi_conf_1280x1024p85,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_148_5		// hdmiphy_preset_157_5
	}, {
		.preset = &hdmi_conf_1360x768p120,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_148_352		// hdmiphy_preset_148_25
	}, {
		.preset = &hdmi_conf_1400x1050p75,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_148_5		// hdmiphy_preset_156
	}, {
		.preset = &hdmi_conf_1440x900p85,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_148_5		// hdmiphy_preset_157
	}, {
		.preset = &hdmi_conf_1680x1050p60,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_148_352		// hdmiphy_preset_146_25
	}, {
		.preset = &hdmi_conf_1920x1200p60,
		.format = &_format_2d,
		.phy_data = hdmiphy_preset_148_5		// hdmiphy_preset_154
	}
};

const int num_hdmi_presets = ARRAY_SIZE(hdmi_conf);
