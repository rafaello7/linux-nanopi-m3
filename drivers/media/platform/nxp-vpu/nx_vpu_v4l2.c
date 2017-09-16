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

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/interrupt.h>

#include <linux/videodev2.h>
#include <linux/videodev2_nxp_media.h>

#include <linux/soc/nexell/nx-media-device.h>

#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#ifdef CONFIG_ARM_S5Pxx18_DEVFREQ
#include <linux/pm_qos.h>
#include <linux/soc/nexell/cpufreq.h>
#endif

#include "nx_vpu_v4l2.h"
#include "vpu_hw_interface.h"

#define	NX_VIDEO_NAME "nx-vpu"
#define	NX_VIDEO_ENC_NAME "nx-vpu-enc"
#define	NX_VIDEO_DEC_NAME "nx-vpu-dec"

static bool single_plane_mode;
module_param(single_plane_mode, bool, 0444);

static unsigned additional_buffer_count = 2;
module_param(additional_buffer_count, uint, 0644);
MODULE_PARM_DESC(additional_buffer_count,
		"number of buffers above minimum required by coda vpu");

#define INFO_MSG		0

#ifdef CONFIG_ARM_S5Pxx18_DEVFREQ
static struct pm_qos_request nx_vpu_qos;

static void nx_vpu_qos_update(int val)
{
	if (!pm_qos_request_active(&nx_vpu_qos))
		pm_qos_add_request(&nx_vpu_qos, PM_QOS_BUS_THROUGHPUT, val);
	else
		pm_qos_update_request(&nx_vpu_qos, val);
}
#endif

dma_addr_t nx_vpu_mem_plane_addr(struct vb2_buffer *v, unsigned n)
{
#ifdef USE_ION_MEMORY
	void *cookie = vb2_plane_cookie(v, n);
	dma_addr_t addr = 0;

	WARN_ON(vb2_ion_dma_address(cookie, &addr) != 0);
	return (unsigned long)addr;
#else
	return vb2_dma_contig_plane_dma_addr(v, n);
#endif
}

int nx_vpu_enc_try_run(struct nx_vpu_ctx *ctx)
{
	struct nx_vpu_v4l2 *dev = ctx->dev;
	struct vpu_enc_ctx *enc_ctx = &ctx->codec.enc;
	unsigned int ret = 0;
	void *err = (void *)(&dev->plat_dev->dev);

	FUNC_IN();

	NX_DbgMsg(INFO_MSG, ("cmd = %x\n", enc_ctx->vpu_cmd));

	mutex_lock(&dev->vpu_mutex);

#ifdef ENABLE_CLOCK_GATING
	NX_VPU_Clock(1);
#endif

	__set_bit(ctx->idx, &dev->ctx_work_bits);

	switch (enc_ctx->vpu_cmd) {
	case GET_ENC_INSTANCE:
		dev->curr_ctx = ctx->idx;
		ret = vpu_enc_open_instance(ctx);
		if (ret != 0)
			dev_err(err, "Failed to create a new instance\n");
		else
			enc_ctx->vpu_cmd = ENC_RUN;
		break;

	case ENC_RUN:
		if (enc_ctx->is_initialized == 0) {
			ret = vpu_enc_init(ctx);
			if (ret != 0)
				dev_err(err, "enc_init failed, ret=%d\n", ret);
			else{
				enc_ctx->is_initialized = 1;
				vpu_enc_get_seq_info(ctx);
			}
		} else {
			ret = vpu_enc_encode_frame(ctx);
			if (ret != 0) {
				dev_err(err, "encode_frame is failed, ret=%d\n",
					ret);
				break;
			}
		}
		break;

	case SEQ_END:
		if (enc_ctx->is_initialized) {
			dev->curr_ctx = ctx->idx;
			nx_vpu_enc_close_instance(ctx);
			enc_ctx->is_initialized = 0;
		}
		break;

	default:
		ret = -EAGAIN;
	}

	__clear_bit(ctx->idx, &dev->ctx_work_bits);

#ifdef ENABLE_CLOCK_GATING
	NX_VPU_Clock(0);
#endif

	mutex_unlock(&dev->vpu_mutex);

	return ret;
}

int nx_vpu_dec_try_cmd(struct nx_vpu_ctx *ctx, enum nx_vpu_cmd vpu_cmd)
{
	struct nx_vpu_v4l2 *dev = ctx->dev;
	struct vpu_dec_ctx *dec_ctx = &ctx->codec.dec;
	unsigned int ret = 0;
	void *err = (void *)(&dev->plat_dev->dev);

	FUNC_IN();

	NX_DbgMsg(INFO_MSG, ("cmd = %x\n", vpu_cmd));

	mutex_lock(&dev->vpu_mutex);

#ifdef ENABLE_CLOCK_GATING
	NX_VPU_Clock(1);
#endif

	__set_bit(ctx->idx, &dev->ctx_work_bits);

	switch( vpu_cmd ) {
	case GET_DEC_INSTANCE:
		if( dec_ctx->state == NX_VPUDEC_CLOSED ) {
			dev->curr_ctx = ctx->idx;
			ret = vpu_dec_open_instance(ctx);
			if (ret != 0)
				dev_err(err, "Failed to create a new instance.\n");
			else{
				ret = vpu_dec_parse_vid_cfg(ctx, single_plane_mode);
				if (ret != 0) {
					dev_err(err, "vpu_dec_parse_vfg error %d", ret);
					nx_vpu_dec_close_instance(ctx);
				}else{
					dec_ctx->state = NX_VPUDEC_SET_FRAMEBUF;
				}
			}
		}else{
			dev_err(err, "GET_DEC_INSTANCE: already initialized\n");
		}
		break;

	case DEC_RUN:
		switch( dec_ctx->state ) {
		case NX_VPUDEC_SET_FRAMEBUF:
			ret = vpu_dec_init(ctx);
			if (ret != 0)
				dev_err(err, "vpu_dec_init failed, ret=%d\n", ret);
			dec_ctx->state = NX_VPUDEC_RUNNING;
			break;
		case NX_VPUDEC_RUNNING:
			ret = vpu_dec_decode_slice(ctx, false);
			if (ret != 0)
				dev_err(err, "vpu_dec_decode_slice failed, err=%d", ret);
			break;
		default:
			break;
		}
		break;

	case DEC_BUF_FLUSH:
		switch( dec_ctx->state ) {
		case NX_VPUDEC_SET_FRAMEBUF:
			ret = NX_VpuDecFlush(ctx->hInst);
			if( ret )
				dev_err(err, "NX_VpuDecFlush err=%d", ret);
			dec_ctx->state = NX_VPUDEC_FLUSHED;
			break;
		case NX_VPUDEC_RUNNING:
			ret = vpu_dec_decode_slice(ctx, true);
			if( ret )
				dev_err(err, "vpu_dec_decode_slice err=%d", ret);
			dec_ctx->state = NX_VPUDEC_FLUSHED;
			break;
		default:
			break;
		}
		break;

	case SEQ_END:
		if( dec_ctx->state == NX_VPUDEC_SET_FRAMEBUF ||
				dec_ctx->state == NX_VPUDEC_RUNNING )
		{
			ret = NX_VpuDecFlush(ctx->hInst);
			if( ret )
				dev_err(err, "NX_VpuDecFlush err=%d", ret);
		}
		if( dec_ctx->state != NX_VPUDEC_CLOSED ) {
			dev->curr_ctx = ctx->idx;
			nx_vpu_dec_close_instance(ctx);
			dec_ctx->state = NX_VPUDEC_CLOSED;
		}
		break;

	default:
		ret = -EAGAIN;
	}

	__clear_bit(ctx->idx, &dev->ctx_work_bits);

#ifdef ENABLE_CLOCK_GATING
	NX_VPU_Clock(0);
#endif

	mutex_unlock(&dev->vpu_mutex);

	return ret;
}


/*-----------------------------------------------------------------------------
 *      functions for Input/Output format
 *----------------------------------------------------------------------------*/
static const struct nx_vpu_image_fmt image_formats[] = {
	{
		.fourcc = V4L2_PIX_FMT_YUV420M,
		.hsub = 2, .vsub = 2,
		.chromaInterleave = false,
		.singleBuffer = false,
	},
	{
		.fourcc = V4L2_PIX_FMT_YUV420,
		.hsub = 2, .vsub = 2,
		.chromaInterleave = false,
		.singleBuffer = true,
	},
#if 0
	{
		.fourcc = V4L2_PIX_FMT_YUV422P,
		.hsub = 2, .vsub = 1,
		.chromaInterleave = false,
		.singleBuffer = true,
	},
	{
		.fourcc = V4L2_PIX_FMT_YUV444,
		.hsub = 1, .vsub = 1,
		.chromaInterleave = false,
		.singleBuffer = true,
	},
	{
		.fourcc = V4L2_PIX_FMT_GREY,
		.hsub = 0, .vsub = 0,
		.singleBuffer = true,
	},
	{
		.fourcc = V4L2_PIX_FMT_YUV422M,
		.hsub = 2, .vsub = 1,
		.chromaInterleave = false,
		.singleBuffer = false,
	},
	{
		.fourcc = V4L2_PIX_FMT_YUV444M,
		.hsub = 1, .vsub = 1,
		.chromaInterleave = false,
		.singleBuffer = false,
	},
	{
		.fourcc = V4L2_PIX_FMT_NV12M,
		.hsub = 2, .vsub = 2,
		.chromaInterleave = true,
		.singleBuffer = false,
	},
	{
		.fourcc = V4L2_PIX_FMT_NV21M,
		.hsub = 2, .vsub = 2,
		.chromaInterleave = true,
		.singleBuffer = false,
	},
	{
		.fourcc = V4L2_PIX_FMT_NV16M,
		.hsub = 2, .vsub = 1,
		.chromaInterleave = true,
		.singleBuffer = false,
	},
	{
		.fourcc = V4L2_PIX_FMT_NV61M,
		.hsub = 2, .vsub = 1,
		.chromaInterleave = true,
		.singleBuffer = false,
	},
	{
		.fourcc = V4L2_PIX_FMT_NV24M,
		.hsub = 1, .vsub = 1,
		.chromaInterleave = true,
		.singleBuffer = false,
	},
	{
		.fourcc = V4L2_PIX_FMT_NV42M,
		.hsub = 1, .vsub = 1,
		.chromaInterleave = true,
		.singleBuffer = false,
	},
#endif
};

static const struct nx_vpu_stream_fmt stream_formats[] = {
	{
		.fourcc = V4L2_PIX_FMT_MPEG2,
	},
	{
		.fourcc = V4L2_PIX_FMT_MPEG4,
	},
	{
		.fourcc = V4L2_PIX_FMT_XVID,
	},
	{
		.fourcc = V4L2_PIX_FMT_DIV3,
	},
	{
		.fourcc = V4L2_PIX_FMT_DIV4,
	},
	{
		.fourcc = V4L2_PIX_FMT_DIV5,
	},
	{
		.fourcc = V4L2_PIX_FMT_DIV6,
	},
	{
		.fourcc = V4L2_PIX_FMT_DIVX,
	},
	{
		.fourcc = V4L2_PIX_FMT_H263,
	},
	{
		.fourcc = V4L2_PIX_FMT_H264,
	},
	{
		.fourcc = V4L2_PIX_FMT_WMV9,
	},
	{
		.fourcc = V4L2_PIX_FMT_WVC1,
	},
	{
		.fourcc = V4L2_PIX_FMT_RV8,
	},
	{
		.fourcc = V4L2_PIX_FMT_RV9,
	},
	{
		.fourcc = V4L2_PIX_FMT_VP8,
	},
	{
		.fourcc = V4L2_PIX_FMT_FLV1,
	},
	{
		.fourcc = V4L2_PIX_FMT_THEORA,
	},
	{
		.fourcc = V4L2_PIX_FMT_MJPEG,
	},
};

const struct nx_vpu_image_fmt *nx_find_image_format(unsigned fourcc)
{
	unsigned int i;

	FUNC_IN();

	for (i = 0 ; i < ARRAY_SIZE(image_formats); i++) {
		if (image_formats[i].fourcc == fourcc)
			return &image_formats[i];
	}
	return NULL;
}

const struct nx_vpu_stream_fmt *nx_find_stream_format(struct v4l2_format *f)
{
	unsigned int i;

	FUNC_IN();

	for (i = 0 ; i < ARRAY_SIZE(stream_formats); i++) {
		if (stream_formats[i].fourcc == f->fmt.pix_mp.pixelformat)
			return &stream_formats[i];
	}
	return NULL;
}

/*-----------------------------------------------------------------------------
 *      functions for vidioc_queryctrl
 *----------------------------------------------------------------------------*/

/* Query capabilities of the device */
int vidioc_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	struct nx_vpu_v4l2 *dev = video_drvdata(file);
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);

	FUNC_IN();

	strncpy(cap->driver, dev->plat_dev->name, sizeof(cap->driver) - 1);
	strncpy(cap->card, dev->plat_dev->name, sizeof(cap->card) - 1);
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 dev_name(&dev->plat_dev->dev));
	cap->device_caps = (ctx->vq_strm.type == V4L2_BUF_TYPE_VIDEO_OUTPUT ?
			V4L2_CAP_VIDEO_M2M : V4L2_CAP_VIDEO_M2M_MPLANE) |
		V4L2_CAP_STREAMING;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

int nx_vidioc_enum_fmt_vid_image(struct file *file, void *pirv,
	struct v4l2_fmtdesc *f)
{
	int i, j = -1;

	for( i = 0; i < ARRAY_SIZE(image_formats); ++i ) {
		if( image_formats[i].singleBuffer ) {
			if( ++j == f->index ) {
				f->pixelformat = image_formats[i].fourcc;
				return 0;
			}
		}
	}
	return -EINVAL;
}

int nx_vidioc_enum_fmt_vid_image_mplane(struct file *file, void *pirv,
	struct v4l2_fmtdesc *f)
{
	if( f->index >= ARRAY_SIZE(image_formats) )
		return -EINVAL;
	f->pixelformat = image_formats[f->index].fourcc;
	return 0;
}

int nx_vidioc_enum_fmt_vid_stream(struct file *file, void *prov,
	struct v4l2_fmtdesc *f)
{
	if( f->index >= ARRAY_SIZE(stream_formats) )
		return -EINVAL;
	f->pixelformat = stream_formats[f->index].fourcc;
	return 0;
}

int nx_vidioc_enum_fmt_vid_stream_mplane(struct file *file, void *priv,
	struct v4l2_fmtdesc *f)
{
	if( f->index >= ARRAY_SIZE(stream_formats) )
		return -EINVAL;
	f->pixelformat = stream_formats[f->index].fourcc;
	return 0;
}

int nx_vidioc_enum_framesizes(struct file *file, void *priv,
				      struct v4l2_frmsizeenum *fsize)
{
	if( fsize->index != 0 )
		return -EINVAL;
	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width = 8;
	fsize->stepwise.max_width = 1920;
	fsize->stepwise.step_width = 8;
	fsize->stepwise.min_height = 8;
	fsize->stepwise.max_height = 1088;
	fsize->stepwise.step_height = 2;
	return 0;
}

#define	DST_QUEUE_OFF_BASE	(1 << 30)
int nx_vpu_vidioc_querybuf(struct file *file, void *priv,
		struct v4l2_buffer *buf)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);
	int i, ret = 0;

	FUNC_IN();

	/* if memory is not mmp or userptr return error */
	if ((buf->memory != V4L2_MEMORY_MMAP) &&
		(buf->memory != V4L2_MEMORY_USERPTR) &&
		(buf->memory != V4L2_MEMORY_DMABUF))
		return -EINVAL;

	if( buf->type == ctx->vq_strm.type ) {
		ret = vb2_querybuf(&ctx->vq_strm, buf);
		if (ret != 0) {
			pr_err("error in vb2_querybuf() for E(D)\n");
			return ret;
		}
	} else if( buf->type == ctx->vq_img.type ) {
		ret = vb2_querybuf(&ctx->vq_img, buf);
		if (ret != 0) {
			pr_err("error in vb2_querybuf() for E(S)\n");
			return ret;
		}
		/* Adjust MMAP memory offsets for the CAPTURE queue */
		if( buf->memory == V4L2_MEMORY_MMAP ) {
			if( buf->type == V4L2_BUF_TYPE_VIDEO_CAPTURE ) {
				buf->m.offset += DST_QUEUE_OFF_BASE;
			}else{
				for (i = 0; i < buf->length; ++i)
					buf->m.planes[i].m.mem_offset += DST_QUEUE_OFF_BASE;
			}
		}
	} else {
		pr_err("invalid buf type\n");
		return -EINVAL;
	}

	return ret;
}

/* Stream on */
int nx_vpu_vidioc_streamon(struct file *file, void *priv,
			   enum v4l2_buf_type type)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);
	int ret = -EINVAL;

	FUNC_IN();

	if( type == ctx->vq_img.type )
		ret = vb2_streamon(&ctx->vq_img, type);
	else if( type == ctx->vq_strm.type )
		ret = vb2_streamon(&ctx->vq_strm, type);

	return ret;
}

/* Stream off, which equals to a pause */
int nx_vpu_vidioc_streamoff(struct file *file, void *priv,
			    enum v4l2_buf_type type)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);
	int ret = -EINVAL;

	FUNC_IN();

	if( type == ctx->vq_img.type )
		ret = vb2_streamoff(&ctx->vq_img, type);
	else if( type == ctx->vq_strm.type )
		ret = vb2_streamoff(&ctx->vq_strm, type);

	return ret;
}


/*-----------------------------------------------------------------------------
 *      functions for VB2 Contorls(struct "vb2_ops")
 *----------------------------------------------------------------------------*/

int nx_vpu_queue_setup(struct vb2_queue *vq,
			unsigned int *buf_count, unsigned int *plane_count,
			unsigned int psize[], struct device *alloc_devs[])
{
	struct nx_vpu_ctx *ctx = vq->drv_priv;
	int i;

	FUNC_IN();

	if (vq->type == ctx->vq_strm.type ) {
		int planeCount = !ctx->is_encoder || ctx->useSingleBuf ||
			ctx->img_fmt->singleBuffer ? 1 :
			ctx->img_fmt->chromaInterleave ? 2 : 3;

		if( *plane_count == 0 ) {
			*plane_count = planeCount;
			if( *buf_count < 1 )
				*buf_count = 1;
			if (*buf_count > VPU_MAX_BUFFERS)
				*buf_count = VPU_MAX_BUFFERS;
			psize[0] = ctx->strm_buf_size;
		}else{	// checking additional buffers for VIDIOC_CREATE_BUFS
			if( *plane_count != planeCount ) {
				NX_ErrMsg(("strm: plane count mismatch"));
				return -EINVAL;
			}
			if( *buf_count + vq->num_buffers > VPU_MAX_BUFFERS )
				*buf_count = VPU_MAX_BUFFERS - vq->num_buffers;
			if( psize[0] < ctx->strm_buf_size )
				return -EINVAL;
		}
	} else if( vq->type == ctx->vq_img.type ) {
		int planeCount = ctx->is_encoder || ctx->useSingleBuf ||
			ctx->img_fmt->singleBuffer ? 1 :
			ctx->img_fmt->chromaInterleave ? 2 : 3;

		if( *plane_count == 0 ) {
			int minBufCnt = ctx->is_encoder ? 1 :
				ctx->codec.dec.minFrameBufCnt + additional_buffer_count;
			*plane_count = planeCount;
			if (*buf_count < minBufCnt)
				*buf_count = minBufCnt;
			if (*buf_count > VPU_MAX_BUFFERS)
				*buf_count = VPU_MAX_BUFFERS;
			switch( planeCount ) {
			case 1:
				psize[0] = ctx->luma_size + 2 * ctx->chroma_size;
				break;
			case 2:
				psize[0] = ctx->luma_size;
				psize[1] = 2 * ctx->chroma_size;
				break;
			default: // 3
				psize[0] = ctx->luma_size;
				psize[1] = psize[2] = ctx->chroma_size;
				break;
			}
		}else{	// checking additional buffers for VIDIOC_CREATE_BUFS
			if( *plane_count != planeCount ) {
				NX_ErrMsg(("img: plane count mismatch"));
				return -EINVAL;
			}
			if( *buf_count + vq->num_buffers > VPU_MAX_BUFFERS )
				*buf_count = VPU_MAX_BUFFERS - vq->num_buffers;
			switch( planeCount ) {
			case 1:
				if( psize[0] < ctx->luma_size + 2 * ctx->chroma_size )
					return -EINVAL;
				break;
			case 2:
				if(psize[0] < ctx->luma_size || psize[1] < 2 * ctx->chroma_size)
					return -EINVAL;
				break;
			default: // 3
				if( psize[0] < ctx->luma_size || psize[1] < ctx->chroma_size ||
					   	psize[2] < ctx->chroma_size )
					return -EINVAL;
				break;
			}
		}
		ctx->codec.dec.declaredFrameBufferCnt = vq->num_buffers + *buf_count;
	} else {
		NX_ErrMsg(("invalid queue type: %d\n", vq->type));
		return -EINVAL;
	}

	NX_DbgMsg(INFO_MSG, ("buf_count: %d, plane_count: %d\n", *buf_count,
		*plane_count));

	for (i = 0; i < *plane_count; i++)
		NX_DbgMsg(INFO_MSG, ("plane[%d] size=%d\n", i, psize[i]));

	return 0;
}

void nx_vpu_unlock(struct vb2_queue *q)
{
	struct nx_vpu_ctx *ctx = q->drv_priv;

	FUNC_IN();
	mutex_unlock(&ctx->dev->dev_mutex);
}

void nx_vpu_lock(struct vb2_queue *q)
{
	struct nx_vpu_ctx *ctx = q->drv_priv;

	FUNC_IN();
	mutex_lock(&ctx->dev->dev_mutex);
}

int nx_vpu_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct nx_vpu_ctx *ctx = vq->drv_priv;
	int i, num_planes;
	unsigned psize[3];
	bool isStream;

	if( vq != &ctx->vq_img && vq != &ctx->vq_strm ) {
		NX_ErrMsg(("buffer not from my pool"));
		return -EINVAL;
	}
	isStream = ctx->is_encoder == (vq == &ctx->vq_img);
	num_planes = isStream || ctx->useSingleBuf || ctx->img_fmt->singleBuffer ?
		1 : ctx->img_fmt->chromaInterleave ? 2 : 3;
	if (num_planes != vb->num_planes) {
		NX_ErrMsg(("invalid plane number for the format, right=%d, cur=%d\n",
			num_planes, vb->num_planes));
		return -EINVAL;
	}
	for (i = 0; i < num_planes; i++) {
		if (!nx_vpu_mem_plane_addr(vb, i)) {
			NX_ErrMsg(("failed to get %d plane cookie\n", i));
			return -EINVAL;
		}

		NX_DbgMsg(INFO_MSG, ("index: %d, plane[%d] cookie: 0x%08lx\n",
			vb->index, i,
			(unsigned long)nx_vpu_mem_plane_addr(vb, i)));
	}
	if( ! isStream ) {
		switch( num_planes ) {
		case 1:
			psize[0] = ctx->luma_size + 2 * ctx->chroma_size;
			break;
		case 2:
			psize[0] = ctx->luma_size;
			psize[1] = 2 * ctx->chroma_size;
			break;
		default: // 3
			psize[0] = ctx->luma_size;
			psize[1] = psize[2] = ctx->chroma_size;
			break;
		}
		for( i = 0; i < num_planes; ++i ) {
			unsigned cur_psize = vb2_plane_size(vb, i);
			if( cur_psize < psize[i] ) {
				NX_ErrMsg(("buffer for plane #%d too small, min=%d cur=%d\n",
							i, psize[i], cur_psize));
				return -EINVAL;
			}
		}
	}
	return 0;
}

void nx_vpu_cleanup_queue(struct list_head *lh, struct vb2_queue *vq,
		enum vb2_buffer_state state)
{
	struct nx_vpu_buf *b;
	int i;

	FUNC_IN();

	while (!list_empty(lh)) {
		b = list_entry(lh->next, struct nx_vpu_buf, list);
		for (i = 0; i < b->vb.num_planes; i++)
			vb2_set_plane_payload(&b->vb, i, 0);
		vb2_buffer_done(&b->vb, state);
		list_del(&b->list);
	}
}

/* -------------------------------------------------------------------------- */


/*-----------------------------------------------------------------------------
 *      Linux VPU/JPU Interrupt Handler
 *----------------------------------------------------------------------------*/

static irqreturn_t nx_vpu_irq(int irq, void *priv)
{
	struct nx_vpu_v4l2 *dev = priv;

	FUNC_IN();
	VpuWriteReg(BIT_INT_CLEAR, 0x1);

	/* Reset the timeout watchdog */
	atomic_set(&dev->vpu_event_present, 1);
	wake_up_interruptible(&dev->vpu_wait_queue);

	return IRQ_HANDLED;
}

static int VPU_WaitVpuInterrupt(struct nx_vpu_v4l2 *dev, int timeOut)
{
	int ret = wait_event_interruptible_timeout(dev->vpu_wait_queue,
		atomic_read(&dev->vpu_event_present),
		msecs_to_jiffies(timeOut));

	if (0 == atomic_read(&dev->vpu_event_present)) {
		/* Error */
		if (ret == 0) {
			NX_ErrMsg(("VPU HW Timeout!\n"));
			atomic_set(&dev->vpu_event_present, 0);
			VPU_SWReset(SW_RESET_SAFETY);
			return -1;
		}

		while (timeOut > 0) {
			if (0 != VpuReadReg(BIT_INT_REASON)) {
				atomic_set(&dev->vpu_event_present, 0);
				return 0;
			}
			DrvMSleep(1);
			timeOut--;
		}

		/* Time out */
		NX_ErrMsg(("VPU HW Error!!\n"));
		VPU_SWReset(SW_RESET_SAFETY);
		atomic_set(&dev->vpu_event_present, 0);
		return -1;
	}

	atomic_set(&dev->vpu_event_present, 0);
	return 0;
}

int VPU_WaitBitInterrupt(void *devHandle, int mSeconds)
{
	unsigned int reason = 0;

#ifdef ENABLE_INTERRUPT_MODE
	if (0 != VPU_WaitVpuInterrupt(devHandle, mSeconds)) {
		reason = VpuReadReg(BIT_INT_REASON);
		VpuWriteReg(BIT_INT_REASON, 0);
		NX_ErrMsg(("VPU_WaitVpuInterrupt() TimeOut!!!\n"));
		NX_ErrMsg(("reason = 0x%.8x, CurPC(0xBD 0xBF : %x %x %x))\n",
			reason, VpuReadReg(BIT_CUR_PC), VpuReadReg(BIT_CUR_PC),
			VpuReadReg(BIT_CUR_PC)));
		return 0;
	}

	VpuWriteReg(BIT_INT_CLEAR, 1);  /* clear HW signal */
	reason = VpuReadReg(BIT_INT_REASON);
	VpuWriteReg(BIT_INT_REASON, 0);
	return reason;
#else
	while (mSeconds > 0) {
		reason = VpuReadReg(BIT_INT_REASON);
		if (reason != 0) {
			if (reason != (unsigned int)-1)
				VpuWriteReg(BIT_INT_CLEAR, 1);
			/* tell to F/W that HOST received an interrupt. */
			VpuWriteReg(BIT_INT_REASON, 0);
			break;
		}
		DrvMSleep(1);
		mSeconds--;
	}
	return reason;
#endif
}

static irqreturn_t nx_jpu_irq(int irq, void *priv)
{
	struct nx_vpu_v4l2 *dev = priv;
	uint32_t val;

	FUNC_IN();

	val = VpuReadReg(MJPEG_PIC_STATUS_REG);
	if (val != 0)
		VpuWriteReg(MJPEG_PIC_STATUS_REG, val);
	dev->jpu_intr_reason = val;

	/* Reset the timeout watchdog */
	atomic_set(&dev->jpu_event_present, 1);
	wake_up_interruptible(&dev->jpu_wait_queue);

	return IRQ_HANDLED;
}

int JPU_WaitInterrupt(void *devHandle, int timeOut)
{
	struct nx_vpu_v4l2 *dev = (struct nx_vpu_v4l2 *)devHandle;
	uint32_t reason = 0;

#ifdef ENABLE_INTERRUPT_MODE
	if (0 == wait_event_interruptible_timeout(dev->jpu_wait_queue,
		atomic_read(&dev->jpu_event_present),
		msecs_to_jiffies(timeOut))) {
		reason = VpuReadReg(MJPEG_PIC_STATUS_REG);
		NX_ErrMsg(("JPU_WaitInterrupt() TimeOut!!!(reason = 0x%.8x)\n",
			reason));
		VPU_SWReset(SW_RESET_SAFETY);
		return 0;
	}

	atomic_set(&dev->jpu_event_present, 0);
	reason = dev->jpu_intr_reason;
#else
	while (timeOut > 0) {
		DrvMSleep(1);

		reason = VpuReadReg(MJPEG_PIC_STATUS_REG);
		if ((reason & (1<<INT_JPU_DONE)) ||
			(reason & (1<<INT_JPU_ERROR)) ||
			(reason & (1<<INT_JPU_BBC_INTERRUPT)) ||
			(reason & (1<<INT_JPU_BIT_BUF_EMPTY)))
			break;

		if (reason & (1<<INT_JPU_BIT_BUF_FULL)) {
			NX_ErrMsg(("Stream Buffer Too Small!!!"));
			VpuReadReg(MJPEG_PIC_STATUS_REG,
				(1 << INT_JPU_BIT_BUF_FULL));
			return reason;
		}

		timeOut--;
		if (timeOut == 0) {
			NX_ErrMsg(("JPU TimeOut!!!"));
			break;
		}
	}
#endif

	return reason;
}

/* -------------------------------------------------------------------------- */


static int nx_vpu_open(struct file *file)
{
	struct video_device *vdev = video_devdata(file);
	struct nx_vpu_v4l2 *dev = video_drvdata(file);
	struct nx_vpu_ctx *ctx = NULL;
	void *err = (void *)(&dev->plat_dev->dev);
	int ret = 0;

	FUNC_IN();

	if (mutex_lock_interruptible(&dev->vpu_mutex))
		return -ERESTARTSYS;

	ctx = devm_kzalloc(err, sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		NX_ErrMsg(("Not enough memory.\n"));
		ret = -ENOMEM;
		goto err_ctx_mem;
	}

	/* get context number */
	ctx->idx = 0;
	while (dev->ctx[ctx->idx]) {
		ctx->idx++;
		if (ctx->idx >= NX_MAX_VPU_INSTANCE) {
			dev_err(err, "Can't open nx vpu driver!!\n");
			dev_err(err, "CurNumInstance = %d)\n",
				dev->cur_num_instance);
			ret = -EBUSY;
			goto err_no_ctx;
		}
	}

	v4l2_fh_init(&ctx->fh, vdev);
	file->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh);

	ctx->dev = dev;

	INIT_LIST_HEAD(&ctx->img_queue);
	INIT_LIST_HEAD(&ctx->strm_queue);
	ctx->img_queue_cnt = 0;
	ctx->strm_queue_cnt = 0;

	/* Mark context as idle */
	__clear_bit(ctx->idx, &dev->ctx_work_bits);
	dev->ctx[ctx->idx] = ctx;

	if (vdev == dev->vfd_enc) {
		ctx->is_encoder = 1;
		ret = nx_vpu_enc_open(ctx);
	} else {
		ctx->is_encoder = 0;
		ret = nx_vpu_dec_open(ctx, single_plane_mode);
	}
	if (ret)
		goto err_ctx_init;

	/* FW Download, HW Init, Clock Set */
	if (dev->cur_num_instance == 0) {
#ifdef CONFIG_ARM_S5Pxx18_DEVFREQ
		nx_vpu_qos_update(NX_BUS_CLK_VPU_KHZ);
#endif
#ifdef ENABLE_POWER_SAVING
		dev->curr_ctx = ctx->idx;

		NX_VPU_Clock(1);
		ret = NX_VpuInit(dev, dev->regs_base,
			dev->firmware_buf->virAddr,
			(uint32_t)dev->firmware_buf->phyAddr);

#ifdef ENABLE_CLOCK_GATING
		NX_VPU_Clock(0);
#endif

		if (ret)
			goto err_hw_init;
#endif
	}

	mutex_unlock(&dev->vpu_mutex);

	return ret;

	/* Deinit when failure occurred */
#ifdef ENABLE_POWER_SAVING
err_hw_init:
#endif
err_ctx_init:
	if (ctx->idx < NX_MAX_VPU_INSTANCE){
		dev->ctx[ctx->idx] = 0;
	}
err_no_ctx:

err_ctx_mem:
	mutex_unlock(&dev->vpu_mutex);

	return ret;
}

static int nx_vpu_close(struct file *file)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);
	struct nx_vpu_v4l2 *dev = ctx->dev;

	FUNC_IN();

	mutex_lock(&dev->dev_mutex);

	if( ctx->is_encoder ) {
		if(ctx->codec.enc.is_initialized) {
			ctx->codec.enc.vpu_cmd = SEQ_END;
			nx_vpu_enc_try_run(ctx);
		}
	}else{
		if(ctx->codec.dec.state != NX_VPUDEC_CLOSED)
			nx_vpu_dec_try_cmd(ctx, SEQ_END);
	}

	if (dev->cur_num_instance == 0) {
#ifdef ENABLE_POWER_SAVING
		/* H/W Power Off */
		NX_VPU_Clock(1);
		NX_VpuDeInit(dev);
#endif
#ifdef CONFIG_ARM_S5Pxx18_DEVFREQ
		nx_vpu_qos_update(NX_BUS_CLK_IDLE_KHZ);
#endif
	}

#ifdef ENABLE_CLOCK_GATING
	NX_VPU_Clock(0);
#endif

	vb2_queue_release(&ctx->vq_img);
	vb2_queue_release(&ctx->vq_strm);

	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);

	/* Mark context as idle */
	__clear_bit(ctx->idx, &dev->ctx_work_bits);

	dev->ctx[ctx->idx] = 0;

	mutex_unlock(&dev->dev_mutex);

	return 0;
}

static unsigned int nx_vpu_poll(struct file *file, struct poll_table_struct
	*wait)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);
	unsigned ret;

	ret = vb2_poll(&ctx->vq_img, file, wait);
	ret |= vb2_poll(&ctx->vq_strm, file, wait);
	return ret;
}

static int nx_vpu_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);
	struct nx_vpu_v4l2 *dev = ctx->dev;
	uint32_t offset = vma->vm_pgoff << PAGE_SHIFT;
	int ret;
	bool isStream;

	FUNC_IN();

	if (mutex_lock_interruptible(&dev->dev_mutex))
		return -ERESTARTSYS;

	if (offset < DST_QUEUE_OFF_BASE) {
		isStream = !ctx->is_encoder;
	} else {
		vma->vm_pgoff -= (DST_QUEUE_OFF_BASE >> PAGE_SHIFT);
		isStream = ctx->is_encoder;
	}
	ret = vb2_mmap(isStream ? &ctx->vq_strm : &ctx->vq_img, vma);

	mutex_unlock(&dev->dev_mutex);

	return ret;
}

static const struct v4l2_file_operations nx_vpu_fops = {
	.owner = THIS_MODULE,
	.open = nx_vpu_open,
	.release = nx_vpu_close,
	.poll = nx_vpu_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = nx_vpu_mmap,
};

void vpu_soc_peri_reset_enter(void *pv)
{
	struct nx_vpu_v4l2 *dev = (struct nx_vpu_v4l2 *)pv;

	reset_control_assert(dev->coda_c);
	reset_control_assert(dev->coda_a);
	reset_control_assert(dev->coda_p);
}

void vpu_soc_peri_reset_exit(void *pv)
{
	struct nx_vpu_v4l2 *dev = (struct nx_vpu_v4l2 *)pv;

	reset_control_deassert(dev->coda_c);
	reset_control_deassert(dev->coda_a);
	reset_control_deassert(dev->coda_p);
}

static int nx_vpu_init(struct nx_vpu_v4l2 *dev)
{
	int ret = 0;

	FUNC_IN();

	dev->firmware_buf = nx_alloc_memory(&dev->plat_dev->dev,
		COMMON_BUF_SIZE, 4096);
	if (dev->firmware_buf == NULL) {
		dev_err(&dev->plat_dev->dev, "firmware allocation is failed!\n");
		return -ENOMEM;
	}

	mutex_lock(&dev->vpu_mutex);

	NX_VPU_Clock(1);

	ret = NX_VpuInit(dev, dev->regs_base, dev->firmware_buf->virAddr,
		dev->firmware_buf->phyAddr);

#ifdef ENABLE_CLOCK_GATING
	NX_VPU_Clock(0);
#endif

	mutex_unlock(&dev->vpu_mutex);

	dev->cur_num_instance = 0;
	dev->cur_jpg_instance = 0;

	return ret;
}

static int nx_vpu_deinit(struct nx_vpu_v4l2 *dev)
{
	int ret;

	FUNC_IN();

	NX_VPU_Clock(1);
	ret = NX_VpuDeInit(dev);
#ifdef ENABLE_CLOCK_GATING
	NX_VPU_Clock(0);
#endif

	if (dev->firmware_buf != NULL)
		nx_free_memory(dev->firmware_buf);

	return ret;
}

static int nx_vpu_probe(struct platform_device *pdev)
{
	struct nx_vpu_v4l2 *dev;
	struct video_device *vfd;
	struct resource res;
	int ret;
	uint32_t info[2] = { };

	FUNC_IN();

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		NX_ErrMsg(("fail to kzalloc(size %zu) (%s)\n",
			sizeof(struct nx_vpu_v4l2), NX_VIDEO_NAME));
		return -ENOMEM;
	}

	spin_lock_init(&dev->irqlock);

	dev->plat_dev = pdev;
	if (!dev->plat_dev) {
		dev_err(&pdev->dev, "No platform data specified\n");
		return -ENODEV;
	}

	ret = of_address_to_resource(pdev->dev.of_node, 0, &res);
	if (ret) {
		dev_err(&pdev->dev, "failed to get base address\n");
		return -ENXIO;
	}

	dev->regs_base = devm_ioremap_nocache(&pdev->dev, res.start,
		resource_size(&res));
	if (!dev->regs_base) {
		dev_err(&pdev->dev, "failed to ioremap\n");
		return -EBUSY;
	}

	/* For VPU interrupt */
	dev->vpu_irq = platform_get_irq(pdev, 0);
	if (dev->vpu_irq < 0) {
		dev_err(&pdev->dev, "failed to get vpu-irq num\n");
		return -EBUSY;
	}

	ret = devm_request_irq(&pdev->dev, dev->vpu_irq, nx_vpu_irq, 0, pdev->name, dev);
	if (ret != 0) {
		dev_err(&pdev->dev, "Failed to install vpu-irq (%d)\n", ret);
		return ret;
	}
	init_waitqueue_head(&dev->vpu_wait_queue);

	/* For JPU interrupt */
	dev->jpu_irq = platform_get_irq(pdev, 1);
	if (dev->jpu_irq < 0) {
		dev_err(&pdev->dev, "failed to get jpu-irq num\n");
		return -EBUSY;
	}

	ret = devm_request_irq(&pdev->dev, dev->jpu_irq, nx_jpu_irq, 0, pdev->name, dev);
	if (ret != 0) {
		dev_err(&pdev->dev, "Failed to install jpu-irq (%d)\n", ret);
		return ret;
	}
	init_waitqueue_head(&dev->jpu_wait_queue);

	dev->coda_c = devm_reset_control_get(&pdev->dev, "vpu-c-reset");
	if (IS_ERR(dev->coda_c)) {
		dev_err(&pdev->dev, "failed to get reset control of vpu-c\n");
		return -ENODEV;
	}

	dev->coda_a = devm_reset_control_get(&pdev->dev, "vpu-a-reset");
	if (IS_ERR(dev->coda_a)) {
		dev_err(&pdev->dev, "failed to get reset control of vpu-c\n");
		return -ENODEV;
	}

	dev->coda_p = devm_reset_control_get(&pdev->dev, "vpu-p-reset");
	if (IS_ERR(dev->coda_p)) {
		dev_err(&pdev->dev, "failed to get reset control of vpu-c\n");
		return -ENODEV;
	}

	ret = of_property_read_u32_array(pdev->dev.of_node, "sram", info, 2);
	if (!ret) {
		dev->sram_base_addr = info[0];
		dev->sram_size = info[1];
	}

	mutex_init(&dev->dev_mutex);
	mutex_init(&dev->vpu_mutex);

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret) {
		dev_err(&pdev->dev, "%s: failed to register v4l2_device: %d\n",
			__func__, ret);
		goto err_v4l2_dev_reg;
	}

	platform_set_drvdata(pdev, dev);

	atomic_set(&dev->vpu_event_present, 0);
	atomic_set(&dev->jpu_event_present, 0);

	ret = NX_VpuParaInitialized(&pdev->dev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to ioremap\n");
		goto err_vpu_init;
	}

	ret = nx_vpu_init(dev);
	if (ret < 0) {
		dev_err(&pdev->dev, "nx_vpu_init() is Failed\n");
		goto err_vpu_init;
	}

	/* encoder */
	vfd = video_device_alloc();
	if (!vfd) {
		dev_err(&pdev->dev, "Fail to allocate video device\n");
		ret = -ENOMEM;
		goto err_enc_alloc;
	}

	vfd->fops = &nx_vpu_fops;
	vfd->ioctl_ops = get_enc_ioctl_ops();
	vfd->minor = -1;
	vfd->release = video_device_release;
	vfd->lock = &dev->dev_mutex;
	vfd->v4l2_dev = &dev->v4l2_dev;
	vfd->vfl_dir = VFL_DIR_M2M;
	snprintf(vfd->name, sizeof(vfd->name), "%s", NX_VIDEO_ENC_NAME);
	dev->vfd_enc = vfd;

	v4l2_info(&dev->v4l2_dev, "encoder registered as /dev/video%d\n",
		NX_VPU_START);
	video_set_drvdata(vfd, dev);
	ret = video_register_device(vfd, VFL_TYPE_GRABBER, NX_VPU_START);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register video device\n");
		goto err_enc_reg;
	}

	/* decoder */
	vfd = video_device_alloc();
	if (!vfd) {
		dev_err(&pdev->dev, "Fail to allocate video device\n");
		ret = -ENOMEM;
		goto err_dec_alloc;
	}

	vfd->fops = &nx_vpu_fops;
	vfd->ioctl_ops = get_dec_ioctl_ops(single_plane_mode);
	vfd->minor = -1;
	vfd->release = video_device_release;
	vfd->lock = &dev->dev_mutex;
	vfd->v4l2_dev = &dev->v4l2_dev;
	vfd->vfl_dir = VFL_DIR_M2M;
	snprintf(vfd->name, sizeof(vfd->name), "%s", NX_VIDEO_DEC_NAME);
	dev->vfd_dec = vfd;

	v4l2_info(&dev->v4l2_dev, "decoder registered as /dev/video%d\n",
		NX_VPU_START + 1);
	video_set_drvdata(vfd, dev);
	ret = video_register_device(vfd, VFL_TYPE_GRABBER, NX_VPU_START + 1);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register video device\n");
		goto err_dec_reg;
	}

	return 0;

	//video_unregister_device(dev->vfd_dec);
err_dec_reg:
	video_device_release(dev->vfd_dec);
err_dec_alloc:
	video_unregister_device(dev->vfd_enc);
err_enc_reg:
	video_device_release(dev->vfd_enc);
err_enc_alloc:
err_vpu_init:
err_v4l2_dev_reg:
	v4l2_device_unregister(&dev->v4l2_dev);

	dev_err(&pdev->dev, "%s-- with error!!!\n", __func__);
	return ret;
}

static int nx_vpu_remove(struct platform_device *pdev)
{
	struct nx_vpu_v4l2 *dev = platform_get_drvdata(pdev);

	FUNC_IN();

	if (unlikely(!dev))
		return 0;

	if (dev->cur_num_instance > 0) {
		dev_err(&pdev->dev, "Warning Video Frimware is running.\n");
		dev_err(&pdev->dev, "(Video(%d), Jpeg(%d)\n",
			dev->cur_num_instance, dev->cur_jpg_instance);
	}

	video_unregister_device(dev->vfd_enc);
	video_unregister_device(dev->vfd_dec);
	v4l2_device_unregister(&dev->v4l2_dev);

	nx_vpu_deinit(dev);

	mutex_destroy(&dev->vpu_mutex);
	mutex_destroy(&dev->dev_mutex);
	devm_free_irq(&dev->plat_dev->dev, dev->jpu_irq, dev);
	devm_free_irq(&dev->plat_dev->dev, dev->vpu_irq, dev);

	return 0;
}

static int nx_vpu_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct nx_vpu_v4l2 *dev = platform_get_drvdata(pdev);

	FUNC_IN();

	mutex_lock(&dev->vpu_mutex);
	NX_VPU_Clock(1);

	NX_VpuSuspend(dev);

#ifdef ENABLE_CLOCK_GATING
	NX_VPU_Clock(0);
#endif
	mutex_unlock(&dev->vpu_mutex);

	FUNC_OUT();
	return 0;
}

static int nx_vpu_resume(struct platform_device *pdev)
{
	struct nx_vpu_v4l2 *dev = platform_get_drvdata(pdev);

	FUNC_IN();

	mutex_lock(&dev->vpu_mutex);
	NX_VPU_Clock(1);

	NX_VpuResume(dev, dev->regs_base);

#ifdef ENABLE_CLOCK_GATING
	NX_VPU_Clock(0);
#endif
	mutex_unlock(&dev->vpu_mutex);

	FUNC_OUT();
	return 0;
}

static struct platform_device_id nx_vpu_driver_ids[] = {
	{
		.name = NX_VIDEO_NAME, .driver_data = 0,
	},
	{},
};

static const struct of_device_id nx_vpu_dt_match[] = {
	{
	.compatible = "nexell, nx-vpu",
	},
	{},
};
MODULE_DEVICE_TABLE(of, nx_vpu_dt_match);

static struct platform_driver nx_vpu_driver = {
	.probe = nx_vpu_probe,
	.remove = nx_vpu_remove,
	.suspend = nx_vpu_suspend,
	.resume = nx_vpu_resume,
	.id_table = nx_vpu_driver_ids,
	.driver = {
		.name = NX_VIDEO_NAME,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(nx_vpu_dt_match),
	},
};

module_platform_driver(nx_vpu_driver);

MODULE_AUTHOR("Kim SeongHee <kshblue@nexell.co.kr>");
MODULE_DESCRIPTION("Nexell S5P6818 series SoC V4L2/Codec device driver");
MODULE_LICENSE("GPL");
