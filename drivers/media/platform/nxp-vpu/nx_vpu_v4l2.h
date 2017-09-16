/*
 * Copyright (C) 2016  Nexell Co., Ltd.
 * Author: Seonghee, Kim <kshblue@nexell.co.kr>
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

#ifndef _nx_vpu_v4l2_H
#define _nx_vpu_v4l2_H

#include <linux/platform_device.h>
#include <linux/irqreturn.h>
#include <media/media-device.h>
#include <media/v4l2-device.h>

#include "nx_port_func.h"
#include "nx_vpu_config.h"
#include "nx_vpu_api.h"


#define STREAM_BUF_SIZE                 (4*1024*1024)
#define ENABLE_INTERRUPT_MODE

#define fh_to_ctx(__fh) container_of(__fh, struct nx_vpu_ctx, fh)
#define vb_to_vpu_buf(x) container_of(x, struct nx_vpu_buf, vb)


struct nx_vpu_v4l2 {
	struct v4l2_device v4l2_dev;
	struct video_device *vfd_dec;
	struct video_device *vfd_enc;
	struct platform_device *plat_dev;

	void *regs_base;
	struct reset_control *coda_c;
	struct reset_control *coda_a;
	struct reset_control *coda_p;

	uint32_t sram_base_addr;
	uint32_t sram_size;

	spinlock_t irqlock;	/* lock when operating on videobuf2 queues */
	struct mutex dev_mutex;
	struct mutex vpu_mutex;

	wait_queue_head_t vpu_wait_queue;
	wait_queue_head_t jpu_wait_queue;

	atomic_t vpu_event_present;
	atomic_t jpu_event_present;

	uint32_t jpu_intr_reason;

	struct nx_vpu_ctx *ctx[NX_MAX_VPU_INSTANCE];
	int curr_ctx;
	unsigned long ctx_work_bits;

	/* instance management */
	int cur_num_instance;
	int cur_jpg_instance;

	int vpu_irq;
	int jpu_irq;

	struct nx_memory_info *firmware_buf;
};

struct vpu_enc_ctx {
	int is_initialized;
	enum nx_vpu_cmd vpu_cmd;

	int gop_frm_cnt;		/* gop frame counter */

	int userIQP;
	int userPQP;

	struct nx_vid_memory_info *ref_recon_buf[2];
	struct nx_memory_info *sub_sample_buf[2];

	union vpu_enc_get_header_arg seq_info;
	struct vpu_enc_seq_arg seq_para;
	struct vpu_enc_run_frame_arg run_info;
	struct vpu_enc_chg_para_arg chg_para;

	int reconChromaInterleave;
};

enum NxVpuDecState {
	NX_VPUDEC_CLOSED,
	NX_VPUDEC_SET_FRAMEBUF,
	NX_VPUDEC_RUNNING,
	NX_VPUDEC_FLUSHED
};

struct vpu_dec_ctx {
	enum NxVpuDecState state;
	int delay_frm;
	int frame_buf_delay;
	int cur_reliable;

	int frm_type[VPU_MAX_BUFFERS];
	int interlace_flg[VPU_MAX_BUFFERS];
	int reliable_0_100[VPU_MAX_BUFFERS];
	u64 timeStamp[VPU_MAX_BUFFERS];
	int multiResolution[VPU_MAX_BUFFERS];
	int upSampledWidth[VPU_MAX_BUFFERS];
	int upSampledHeight[VPU_MAX_BUFFERS];

	struct timeval savedTimeStamp;

	unsigned int start_Addr;
	unsigned int end_Addr;

	int minFrameBufCnt;
	int declaredFrameBufferCnt;
	int frame_buffer_cnt;
	int registeredCount;
	struct vpu_dec_phy_addr_info phyAddrs;

	struct nx_memory_info *col_mv_buf;
	struct nx_memory_info *slice_buf;
	struct nx_memory_info *pv_slice_buf;

	struct nx_vpu_buf *dpb_bufs[VPU_MAX_BUFFERS];
	unsigned int dpb_queue_cnt;

	int crop_left, crop_right, crop_top, crop_bot;

	/* for Jpeg */
	int32_t thumbnailMode;
};

/* YUV image format description - output for decoder, input for encoder.
 * For non-planar formats (GRAY8) only fourcc value is meaningful and
 * singleBuffer, which should be set to true.
 *
 * Planar format may be contiguous (chroma directly after luma, in one
 * buffer) or non-contiguous (in two or three buffers). Chroma may be
 * interleaved (two planes) or not (three planes).
 */
struct nx_vpu_image_fmt {
	unsigned fourcc;
	unsigned hsub, vsub;	// subpixel for planar formats, 0 for non-planar
	bool chromaInterleave;
	bool singleBuffer;		// whether planes are contiguous, in single buffer
};

struct nx_vpu_stream_fmt {
	unsigned int fourcc;
};

struct nx_vpu_ctx {
	struct nx_vpu_v4l2 *dev;
	struct v4l2_fh fh;

	int idx;

	void *hInst;				/* VPU handle */
	int is_encoder;
	int codec_mode;

	int width;
	int height;

	int buf_y_width;
	int buf_c_width;
	int buf_height;

	int luma_size;
	int chroma_size;

	int chromaInterleave;

	unsigned int strm_size;
	unsigned int strm_buf_size;

	struct nx_memory_info *instance_buf;
	struct nx_memory_info *bit_stream_buf;

#if 0
	/* TBD. */
	struct list_head ctrls;
	struct list_head src_ctrls[VPU_MAX_BUFFERS];
	struct list_head dst_ctrls[VPU_MAX_BUFFERS];
	unsigned long src_ctrls_avail;
	unsigned long dst_ctrls_avail;
#endif

	const struct nx_vpu_image_fmt *img_fmt;
	const struct nx_vpu_stream_fmt *strm_fmt;
	bool useSingleBuf;

	struct vb2_queue vq_img;
	struct vb2_queue vq_strm;
	struct list_head img_queue;
	struct list_head strm_queue;
	unsigned int img_queue_cnt;
	unsigned int strm_queue_cnt;

	struct nx_vpu_codec_ops *c_ops;

	union {
		struct vpu_enc_ctx enc;
		struct vpu_dec_ctx dec;
	} codec;
};

struct nx_vpu_buf {
	struct vb2_buffer vb;
	struct list_head list;
};


dma_addr_t nx_vpu_mem_plane_addr(struct vb2_buffer *v, unsigned plane);
int nx_vpu_enc_try_run(struct nx_vpu_ctx *ctx);
int nx_vpu_dec_try_cmd(struct nx_vpu_ctx *ctx, enum nx_vpu_cmd);

const struct nx_vpu_image_fmt *nx_find_image_format(unsigned fourcc);
const struct nx_vpu_stream_fmt *nx_find_stream_format(struct v4l2_format *f);

int vidioc_querycap(struct file *file, void *priv, struct v4l2_capability *cap);
int nx_vidioc_enum_fmt_vid_image(struct file *file, void *pirv,
	struct v4l2_fmtdesc *f);
int nx_vidioc_enum_fmt_vid_image_mplane(struct file *file, void *pirv,
	struct v4l2_fmtdesc *f);
int nx_vidioc_enum_fmt_vid_stream(struct file *file, void *prov,
	struct v4l2_fmtdesc *f);
int nx_vidioc_enum_fmt_vid_stream_mplane(struct file *file, void *priv,
	struct v4l2_fmtdesc *f);
int nx_vidioc_enum_framesizes(struct file *file, void *priv,
				      struct v4l2_frmsizeenum *fsize);
int nx_vpu_vidioc_querybuf(struct file *file, void *priv,
		struct v4l2_buffer *buf);
int nx_vpu_vidioc_streamon(struct file *file, void *priv,
		enum v4l2_buf_type type);
int nx_vpu_vidioc_streamoff(struct file *file, void *priv,
		enum v4l2_buf_type type);
int nx_vpu_queue_setup(struct vb2_queue *vq,
	unsigned int *buf_count, unsigned int *plane_count,
	unsigned int psize[], struct device *alloc_devs[]);

void nx_vpu_unlock(struct vb2_queue *q);
void nx_vpu_lock(struct vb2_queue *q);
int nx_vpu_buf_prepare(struct vb2_buffer *vb);
void nx_vpu_cleanup_queue(struct list_head *lh, struct vb2_queue *vq,
		enum vb2_buffer_state state);

/* For Encoder V4L2 */
const struct v4l2_ioctl_ops *get_enc_ioctl_ops(void);

int nx_vpu_enc_open(struct nx_vpu_ctx *ctx);
int vpu_enc_open_instance(struct nx_vpu_ctx *ctx);
int vpu_enc_init(struct nx_vpu_ctx *ctx);
void vpu_enc_get_seq_info(struct nx_vpu_ctx *ctx);
int vpu_enc_encode_frame(struct nx_vpu_ctx *ctx);
void nx_vpu_enc_close_instance(struct nx_vpu_ctx*);

/* For Decoder V4L2 */
const struct v4l2_ioctl_ops *get_dec_ioctl_ops(bool singlePlaneMode);

int nx_vpu_dec_open(struct nx_vpu_ctx *ctx, bool singlePlaneMode);
int vpu_dec_open_instance(struct nx_vpu_ctx *ctx);
int vpu_dec_parse_vid_cfg(struct nx_vpu_ctx *ctx, bool singlePlaneMode);
int vpu_dec_init(struct nx_vpu_ctx *ctx);
int vpu_dec_decode_slice(struct nx_vpu_ctx *ctx, bool flush);
void nx_vpu_dec_close_instance(struct nx_vpu_ctx*);

#endif          /* #define _nx_vpu_v4l2_H */
