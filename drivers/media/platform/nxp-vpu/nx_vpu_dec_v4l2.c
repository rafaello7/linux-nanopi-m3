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

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/videodev2.h>
#include <linux/videodev2_nxp_media.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/of.h>
#include <linux/interrupt.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>

#include <linux/dma-mapping.h>
#include <linux/dma-buf.h>
#include <linux/cma.h>

#include "vpu_hw_interface.h"
#include "nx_vpu_v4l2.h"


#define INFO_MSG		0

#define PS_SAVE_SIZE (320 * 1024)

static bool holes_for_nxvideodec;
module_param(holes_for_nxvideodec, bool, 0644);
MODULE_PARM_DESC(holes_for_nxvideodec,
		"run in mode for nxvideodec gstreamer plugin");

static int free_decoder_memory(struct nx_vpu_ctx*);

static int nx_vpu_dec_ctx_ready(struct nx_vpu_ctx *ctx)
{
	struct vpu_dec_ctx *dec_ctx = &ctx->codec.dec;

	NX_DbgMsg(INFO_MSG, ("src = %d, dpb = %d\n",
		ctx->strm_queue_cnt, dec_ctx->dpb_queue_cnt));
	switch( dec_ctx->state ) {
	case NX_VPUDEC_SET_FRAMEBUF:
		/* we need all buffers to configure VPU rotator */
		return ctx->vq_img.start_streaming_called &&
			dec_ctx->dpb_queue_cnt >= dec_ctx->declaredFrameBufferCnt;
	case NX_VPUDEC_RUNNING:
		dec_ctx->delay_frm = 1;
		/* VPU refuses to decode slice when lacks at least two buffers from
		 * the reported minimum */
		return ctx->strm_queue_cnt > 0 &&
			dec_ctx->dpb_queue_cnt >= dec_ctx->minFrameBufCnt - 1;
	default:
		return 0;
	}
}

/*-----------------------------------------------------------------------------
 *      functions for Parameter controls
 *----------------------------------------------------------------------------*/
static struct v4l2_queryctrl controls[] = {
	{
		.id = V4L2_CID_MPEG_VIDEO_THUMBNAIL_MODE,
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.name = "Enable thumbnail",
		.minimum = 0,
		.maximum = 1,
		.step = 1,
		.default_value = 0,
	},
};
#define NUM_CTRLS ARRAY_SIZE(controls)

static struct v4l2_queryctrl *get_ctrl(int id)
{
	int i;

	FUNC_IN();

	for (i = 0; i < NUM_CTRLS; ++i)
		if (id == controls[i].id)
			return &controls[i];
	return NULL;
}

static int check_ctrl_val(struct nx_vpu_ctx *ctx, struct v4l2_control *ctrl)
{
	struct v4l2_queryctrl *c;

	FUNC_IN();

	c = get_ctrl(ctrl->id);
	if (!c)
		return -EINVAL;
	if (ctrl->value < c->minimum || ctrl->value > c->maximum
	    || (c->step != 0 && ctrl->value % c->step != 0)) {
		NX_ErrMsg(("Invalid control value\n"));
		NX_ErrMsg(("value = %d, min = %d, max = %d, step = %d\n",
			ctrl->value, c->minimum, c->maximum, c->step));
		return -ERANGE;
	}

	return 0;
}
/* -------------------------------------------------------------------------- */


/*-----------------------------------------------------------------------------
 *      functions for vidioc_queryctrl
 *----------------------------------------------------------------------------*/
static void fill_fmt_width_height(struct v4l2_format *f,
		const struct nx_vpu_image_fmt *fmt,
		unsigned width, unsigned height)
{
	struct v4l2_pix_format *pix = &f->fmt.pix;
	unsigned bytesperline;

	pix->width = width;
	pix->height = height;

	bytesperline = ALIGN(width, 8);
	pix->bytesperline = bytesperline;
	pix->sizeimage = bytesperline * pix->height;
	if( fmt->hsub )
		pix->sizeimage += 2 * bytesperline / fmt->hsub * pix->height / fmt->vsub;
}

static void fill_fmt_width_height_mplane(struct v4l2_format *f,
		const struct nx_vpu_image_fmt *fmt,
		unsigned width, unsigned height)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	unsigned bytesperline;

	pix_mp->width = width;
	pix_mp->height = height;

	bytesperline = ALIGN(width, 8);
	pix_mp->plane_fmt[0].bytesperline = bytesperline;
	pix_mp->plane_fmt[0].sizeimage = bytesperline * pix_mp->height;
	switch( pix_mp->num_planes ) {
	case 1:
		if( fmt->hsub ) {
			pix_mp->plane_fmt[0].sizeimage +=
				2 * bytesperline / fmt->hsub * pix_mp->height / fmt->vsub;
		}
		break;
	case 2:
		pix_mp->plane_fmt[1].bytesperline = bytesperline / fmt->hsub;
		pix_mp->plane_fmt[1].sizeimage =
			2 * bytesperline / fmt->hsub * pix_mp->height / fmt->vsub;
		break;
	default:	// 3
		pix_mp->plane_fmt[1].bytesperline =
			pix_mp->plane_fmt[2].bytesperline = bytesperline / fmt->hsub;
		pix_mp->plane_fmt[1].sizeimage = pix_mp->plane_fmt[2].sizeimage =
			bytesperline / fmt->hsub * pix_mp->height / fmt->vsub;
		break;
	}
}

static int vidioc_g_fmt_vid_cap(struct file *file, void *priv,
	struct v4l2_format *f)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);
	struct v4l2_pix_format *pix = &f->fmt.pix;

	FUNC_IN();

	if( ctx->img_fmt == NULL || ctx->width == 0 || ctx->height == 0 ||
			ctx->codec.dec.minFrameBufCnt == 0 )
	{
			NX_ErrMsg(("There is not cfg information!!"));
			return -EINVAL;
	}

	pix->pixelformat = ctx->img_fmt->fourcc;
	pix->field = ctx->codec.dec.interlace_flg[0];
	fill_fmt_width_height(f, ctx->img_fmt, ctx->width, ctx->height);

	NX_DbgMsg(INFO_MSG, ("vidioc_g_fmt_vid_cap: W = %d, H = %d\n",
		pix->width, pix->height));

	return 0;
}

static int vidioc_g_fmt_vid_cap_mplane(struct file *file, void *priv,
	struct v4l2_format *f)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;

	FUNC_IN();

	if( ctx->img_fmt == NULL || ctx->width == 0 || ctx->height == 0 ||
			ctx->codec.dec.minFrameBufCnt == 0 )
	{
			NX_ErrMsg(("There is not cfg information!!"));
			return -EINVAL;
	}

	pix_mp->num_planes = ctx->useSingleBuf || ctx->img_fmt->hsub == 0 ? 1 :
		ctx->img_fmt->chromaInterleave ? 2 : 3;
	pix_mp->pixelformat = ctx->img_fmt->fourcc;
	pix_mp->field = ctx->codec.dec.interlace_flg[0];
	fill_fmt_width_height_mplane(f, ctx->img_fmt, ctx->width, ctx->height);

	/* TBD. Patch for fedora */
	if (7 == sizeof(pix_mp->reserved)) {
		pix_mp->reserved[0] = (__u8)ctx->codec.dec.minFrameBufCnt;
		pix_mp->reserved[1] = (__u8)ctx->codec.dec.minFrameBufCnt;
	} else if (8 == sizeof(pix_mp->reserved)) {
		pix_mp->reserved[1] = (__u8)ctx->codec.dec.minFrameBufCnt;
		pix_mp->reserved[2] = (__u8)ctx->codec.dec.minFrameBufCnt;
	}

	NX_DbgMsg(INFO_MSG, ("vidioc_g_fmt_vid_cap_mplane : W = %d, H = %d\n",
		pix_mp->width, pix_mp->height));

	return 0;
}

static int vidioc_g_fmt_vid_out(struct file *file, void *priv,
	struct v4l2_format *f)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);
	struct v4l2_pix_format *pix = &f->fmt.pix;

	FUNC_IN();

	NX_DbgMsg(INFO_MSG, ("f->type = %d\n", f->type));

	pix->width = 0;
	pix->height = 0;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = ctx->strm_buf_size;
	pix->sizeimage = ctx->strm_buf_size;
	pix->pixelformat = ctx->strm_fmt->fourcc;

	return 0;
}

static int vidioc_g_fmt_vid_out_mplane(struct file *file, void *priv,
	struct v4l2_format *f)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;

	FUNC_IN();

	NX_DbgMsg(INFO_MSG, ("f->type = %d\n", f->type));

	pix_mp->width = 0;
	pix_mp->height = 0;
	pix_mp->field = V4L2_FIELD_NONE;
	pix_mp->plane_fmt[0].bytesperline = ctx->strm_buf_size;
	pix_mp->plane_fmt[0].sizeimage = ctx->strm_buf_size;
	pix_mp->pixelformat = ctx->strm_fmt->fourcc;
	pix_mp->num_planes = 1;

	return 0;
}

static int nx_vidioc_try_cap_fmt(struct file *file, void *priv,
	struct v4l2_format *f)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);
	struct v4l2_pix_format *pix_fmt = &f->fmt.pix;
	const struct nx_vpu_image_fmt *fmt;

	FUNC_IN();

	fmt = nx_find_image_format(f->fmt.pix.pixelformat);
	if (!fmt) {
		NX_ErrMsg(("capture format %x not found\n",
			pix_fmt->pixelformat));
		return -EINVAL;
	}
	if( ! fmt->singleBuffer ) {
		NX_ErrMsg(("format %.4s is multi-plane, invalid\n",
			   (const char*)&pix_fmt->pixelformat));
		return -EINVAL;
	}

	pix_fmt->field = ctx->codec.dec.interlace_flg[0];
	fill_fmt_width_height(f, fmt, ctx->width, ctx->height);
	return 0;
}

static int nx_vidioc_try_cap_fmt_mplane(struct file *file, void *priv,
	struct v4l2_format *f)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);
	struct v4l2_pix_format_mplane *pix_fmt_mp = &f->fmt.pix_mp;
	const struct nx_vpu_image_fmt *fmt;

	FUNC_IN();

	fmt = nx_find_image_format(f->fmt.pix_mp.pixelformat);
	if (!fmt) {
		NX_ErrMsg(("capture format %x not found\n",
			pix_fmt_mp->pixelformat));
		return -EINVAL;
	}

	/* num_planes equal to 1 means user proposes to store all planes
	 * in single buffer */
	if( pix_fmt_mp->num_planes != 1 )
		pix_fmt_mp->num_planes = fmt->singleBuffer ? 1 :
			fmt->chromaInterleave ? 2 : 3;
	pix_fmt_mp->field = ctx->codec.dec.interlace_flg[0];
	fill_fmt_width_height_mplane(f, fmt, ctx->width, ctx->height);
	return 0;
}

static int nx_vidioc_try_out_fmt(struct file *file, void *priv,
	struct v4l2_format *f)
{
	struct v4l2_pix_format *pix_fmt = &f->fmt.pix;
	const struct nx_vpu_stream_fmt *fmt;

	FUNC_IN();

	fmt = nx_find_stream_format(f);
	if (!fmt) {
		NX_ErrMsg(("out format %x not found\n", pix_fmt->pixelformat));
		return -EINVAL;
	}
	return 0;
}

static int nx_vidioc_try_out_fmt_mplane(struct file *file, void *priv,
	struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_fmt_mp = &f->fmt.pix_mp;
	const struct nx_vpu_stream_fmt *fmt;

	FUNC_IN();

	fmt = nx_find_stream_format(f);
	if (!fmt) {
		NX_ErrMsg(("out format %x not found\n",
			pix_fmt_mp->pixelformat));
		return -EINVAL;
	}
	pix_fmt_mp->num_planes = 1;
	return 0;
}

static int vidioc_s_fmt_vid_cap(struct file *file, void *priv,
	struct v4l2_format *f)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);
	const struct nx_vpu_image_fmt *img_fmt;
	int ret = 0;

	FUNC_IN();

	if (ctx->vq_img.streaming) {
		NX_ErrMsg(("%s queue busy\n", __func__));
		return -EBUSY;
	}

	ret = nx_vidioc_try_cap_fmt(file, priv, f);
	if (ret)
		return ret;

	img_fmt = nx_find_image_format(f->fmt.pix.pixelformat);

	ctx->img_fmt = img_fmt;
	ctx->useSingleBuf = true;

	if( img_fmt->hsub ) {
		ctx->buf_c_width = ctx->buf_y_width / img_fmt->hsub;
		ctx->chroma_size = ctx->buf_c_width * ctx->buf_height / img_fmt->vsub;
	}else{
		ctx->buf_c_width = 0;
		ctx->chroma_size = 0;
	}
	ctx->chromaInterleave = img_fmt->chromaInterleave;

	return 0;
}

static int vidioc_s_fmt_vid_cap_mplane(struct file *file, void *priv,
	struct v4l2_format *f)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);
	struct v4l2_pix_format_mplane *pix_fmt_mp = &f->fmt.pix_mp;
	const struct nx_vpu_image_fmt *img_fmt;
	int ret = 0;

	FUNC_IN();

	if (ctx->vq_img.streaming) {
		NX_ErrMsg(("%s queue busy\n", __func__));
		return -EBUSY;
	}

	ret = nx_vidioc_try_cap_fmt_mplane(file, priv, f);
	if (ret)
		return ret;

	img_fmt = nx_find_image_format(f->fmt.pix_mp.pixelformat);

	ctx->img_fmt = img_fmt;
	ctx->useSingleBuf = pix_fmt_mp->num_planes == 1;

	if( img_fmt->hsub ) {
		ctx->buf_c_width = ctx->buf_y_width / img_fmt->hsub;
		ctx->chroma_size = ctx->buf_c_width * ctx->buf_height / img_fmt->vsub;
	}else{
		ctx->buf_c_width = 0;
		ctx->chroma_size = 0;
	}
	ctx->chromaInterleave = img_fmt->chromaInterleave;

	return 0;
}

static int vidioc_s_fmt_vid_out(struct file *file, void *priv,
	struct v4l2_format *f)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);
	int ret = 0;
	struct v4l2_pix_format *pix_fmt = &f->fmt.pix;

	FUNC_IN();

	if (ctx->vq_strm.streaming) {
		NX_ErrMsg(("%s queue busy\n", __func__));
		return -EBUSY;
	}

	ret = nx_vidioc_try_out_fmt(file, priv, f);
	if (ret)
		return ret;

	ctx->strm_fmt = nx_find_stream_format(f);
	ctx->width = pix_fmt->width;
	ctx->height = pix_fmt->height;

	if (pix_fmt->sizeimage)
		ctx->strm_buf_size = pix_fmt->sizeimage;
	return ret;
}

static int vidioc_s_fmt_vid_out_mplane(struct file *file, void *priv,
	struct v4l2_format *f)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);
	int ret = 0;
	struct v4l2_pix_format_mplane *pix_fmt_mp = &f->fmt.pix_mp;

	FUNC_IN();

	if (ctx->vq_strm.streaming) {
		NX_ErrMsg(("%s queue busy\n", __func__));
		return -EBUSY;
	}

	ret = nx_vidioc_try_out_fmt_mplane(file, priv, f);
	if (ret)
		return ret;

	ctx->strm_fmt = nx_find_stream_format(f);
	ctx->width = pix_fmt_mp->width;
	ctx->height = pix_fmt_mp->height;

	if (pix_fmt_mp->plane_fmt[0].sizeimage)
		ctx->strm_buf_size = pix_fmt_mp->plane_fmt[0].sizeimage;
	return ret;
}

static int vidioc_reqbufs(struct file *file, void *priv,
	struct v4l2_requestbuffers *reqbufs)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);
	int ret;

	if (reqbufs->type == ctx->vq_strm.type ) {
		ret = vb2_reqbufs(&ctx->vq_strm, reqbufs);
	} else if (reqbufs->type == ctx->vq_img.type ) {
		ret = vb2_reqbufs(&ctx->vq_img, reqbufs);
	} else {
		ret = -EINVAL;
	}
	return ret;
}

static int handle_end_of_stream(struct nx_vpu_ctx *ctx)
{
	int ret;

	ret = nx_vpu_dec_try_cmd(ctx, DEC_BUF_FLUSH);

	return ret;
}

static int vidioc_qbuf(struct file *file, void *priv, struct v4l2_buffer *buf)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);

	if (buf->type == ctx->vq_strm.type) {
		unsigned bytesused = buf->type == V4L2_BUF_TYPE_VIDEO_OUTPUT ?
			buf->bytesused : buf->m.planes[0].bytesused;
		if( bytesused == 0 && ctx->codec.dec.state != NX_VPUDEC_CLOSED ) {
			return handle_end_of_stream(ctx);
		} else {
			return vb2_qbuf(&ctx->vq_strm, buf);
		}
	} else {
		return vb2_qbuf(&ctx->vq_img, buf);
	}
}

static int vidioc_dqbuf(struct file *file, void *priv, struct v4l2_buffer *buf)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);
	int ret;

	FUNC_IN();

	if (buf->type == ctx->vq_strm.type ) {
		ret = vb2_dqbuf(&ctx->vq_strm, buf, file->f_flags & O_NONBLOCK);
	} else {
		if ( !holes_for_nxvideodec || ctx->codec.dec.delay_frm == 0) {
			ret = vb2_dqbuf(&ctx->vq_img, buf, file->f_flags &
				O_NONBLOCK);
		} else if (ctx->codec.dec.delay_frm == 1) {
			buf->index = -3;
			ret = 0;
		} else {
			buf->index = -1;
			ret = 0;
		}
	}

	return ret;
}

static int vidioc_g_crop(struct file *file, void *priv, struct v4l2_crop *cr)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);
	struct vpu_dec_ctx *dec_ctx = &ctx->codec.dec;

	FUNC_IN();

	cr->c.left = dec_ctx->crop_left;
	cr->c.top = dec_ctx->crop_top;
	cr->c.width = dec_ctx->crop_right - dec_ctx->crop_left;
	cr->c.height = dec_ctx->crop_bot - dec_ctx->crop_top;

	return 0;
}

/* Query a ctrl */
static int vidioc_queryctrl(struct file *file, void *priv, struct v4l2_queryctrl
	*qc)
{
	struct v4l2_queryctrl *ctrl;

	FUNC_IN();
	ctrl = get_ctrl(qc->id);
	if( ctrl == NULL )
		return -EINVAL;
	*qc = *ctrl;
	return 0;
}

static int vidioc_g_ctrl(struct file *file, void *priv, struct v4l2_control
	*ctrl)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);

	FUNC_IN();
	if (ctrl->id == V4L2_CID_MPEG_VIDEO_THUMBNAIL_MODE) {
		ctrl->value = ctx->codec.dec.thumbnailMode;
	} else {
		NX_DbgMsg(INFO_MSG, ("unsupported control id=0x%x\n", ctrl->id));
		return -EINVAL;
	}
	return 0;
}

static int vidioc_s_ctrl(struct file *file, void *priv, struct v4l2_control
	*ctrl)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);
	int ret = 0;

	FUNC_IN();

	ret = check_ctrl_val(ctx, ctrl);
	if (ret != 0)
		return ret;

	if (ctrl->id == V4L2_CID_MPEG_VIDEO_THUMBNAIL_MODE) {
		ctx->codec.dec.thumbnailMode = ctrl->value;
	} else {
		NX_ErrMsg(("Invalid control(ID = %x)\n", ctrl->id));
		return -EINVAL;
	}

	return ret;
}

static int vidioc_g_ext_ctrls(struct file *file, void *priv,
	struct v4l2_ext_controls *f)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);
	unsigned i;

	FUNC_IN();
	for(i = 0; i < f->count; ++i) {
		if (f->controls[i].id == V4L2_CID_MPEG_VIDEO_THUMBNAIL_MODE) {
			f->controls[i].value = ctx->codec.dec.thumbnailMode;
		} else {
			NX_ErrMsg(("Invalid control(ID = %x)\n", f->controls[i].id));
			return -EINVAL;
		}
	}
	return 0;
}

static int nx_vidioc_expbuf(struct file *file, void *fh,
			     struct v4l2_exportbuffer *e)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);
	if( e->type == ctx->vq_strm.type ) {
		return vb2_expbuf(&ctx->vq_strm, e);
	}else if( e->type == ctx->vq_img.type ) {
		return vb2_expbuf(&ctx->vq_img, e);
	}else{
		pr_err("nx_vidioc_expbuf: bad buffer type %d\n", e->type);
		return -EINVAL;
	}
}

static int nx_vidioc_try_decoder_cmd(struct file *file, void *fh,
				      struct v4l2_decoder_cmd *a)
{
	if( a->cmd == V4L2_DEC_CMD_STOP )
		return 0;
	return -EINVAL;
}

static int nx_vidioc_decoder_cmd(struct file *file, void *fh,
				  struct v4l2_decoder_cmd *a)
{
	struct nx_vpu_ctx *ctx = fh_to_ctx(file->private_data);

	if( a->cmd != V4L2_DEC_CMD_STOP )
		return -EINVAL;
	return handle_end_of_stream(ctx);
}

static const struct v4l2_ioctl_ops nx_vpu_dec_ioctl_ops = {
	.vidioc_querycap = vidioc_querycap,
	.vidioc_enum_fmt_vid_cap = nx_vidioc_enum_fmt_vid_image,
	.vidioc_enum_fmt_vid_out = nx_vidioc_enum_fmt_vid_stream,
	.vidioc_enum_framesizes			= nx_vidioc_enum_framesizes,
	.vidioc_g_fmt_vid_cap			= vidioc_g_fmt_vid_cap,
	.vidioc_g_fmt_vid_out			= vidioc_g_fmt_vid_out,
	.vidioc_try_fmt_vid_cap			= nx_vidioc_try_cap_fmt,
	.vidioc_try_fmt_vid_out			= nx_vidioc_try_out_fmt,
	.vidioc_s_fmt_vid_cap			= vidioc_s_fmt_vid_cap,
	.vidioc_s_fmt_vid_out			= vidioc_s_fmt_vid_out,
	.vidioc_reqbufs = vidioc_reqbufs,
	.vidioc_querybuf = nx_vpu_vidioc_querybuf,
	.vidioc_qbuf = vidioc_qbuf,
	.vidioc_dqbuf = vidioc_dqbuf,
	.vidioc_streamon = nx_vpu_vidioc_streamon,
	.vidioc_streamoff = nx_vpu_vidioc_streamoff,
	.vidioc_queryctrl = vidioc_queryctrl,
	.vidioc_g_ctrl = vidioc_g_ctrl,
	.vidioc_s_ctrl = vidioc_s_ctrl,
	.vidioc_g_crop = vidioc_g_crop,
	.vidioc_g_ext_ctrls = vidioc_g_ext_ctrls,
	.vidioc_expbuf = nx_vidioc_expbuf,
	.vidioc_try_decoder_cmd	= nx_vidioc_try_decoder_cmd,
	.vidioc_decoder_cmd = nx_vidioc_decoder_cmd,
};

static const struct v4l2_ioctl_ops nx_vpu_dec_ioctl_ops_mplane = {
	.vidioc_querycap = vidioc_querycap,
	.vidioc_enum_fmt_vid_cap_mplane = nx_vidioc_enum_fmt_vid_image_mplane,
	.vidioc_enum_fmt_vid_out_mplane = nx_vidioc_enum_fmt_vid_stream_mplane,
	.vidioc_enum_framesizes			= nx_vidioc_enum_framesizes,
	.vidioc_g_fmt_vid_cap_mplane = vidioc_g_fmt_vid_cap_mplane,
	.vidioc_g_fmt_vid_out_mplane = vidioc_g_fmt_vid_out_mplane,
	.vidioc_try_fmt_vid_cap_mplane = nx_vidioc_try_cap_fmt_mplane,
	.vidioc_try_fmt_vid_out_mplane = nx_vidioc_try_out_fmt_mplane,
	.vidioc_s_fmt_vid_cap_mplane = vidioc_s_fmt_vid_cap_mplane,
	.vidioc_s_fmt_vid_out_mplane = vidioc_s_fmt_vid_out_mplane,
	.vidioc_reqbufs = vidioc_reqbufs,
	.vidioc_querybuf = nx_vpu_vidioc_querybuf,
	.vidioc_qbuf = vidioc_qbuf,
	.vidioc_dqbuf = vidioc_dqbuf,
	.vidioc_streamon = nx_vpu_vidioc_streamon,
	.vidioc_streamoff = nx_vpu_vidioc_streamoff,
	.vidioc_queryctrl = vidioc_queryctrl,
	.vidioc_g_ctrl = vidioc_g_ctrl,
	.vidioc_s_ctrl = vidioc_s_ctrl,
	.vidioc_g_crop = vidioc_g_crop,
	.vidioc_g_ext_ctrls = vidioc_g_ext_ctrls,
	.vidioc_expbuf = nx_vidioc_expbuf,
	.vidioc_try_decoder_cmd	= nx_vidioc_try_decoder_cmd,
	.vidioc_decoder_cmd = nx_vidioc_decoder_cmd,
};

/* -------------------------------------------------------------------------- */


static void cleanup_dpb_queue(struct nx_vpu_ctx *ctx,
		enum vb2_buffer_state state)
{
	struct nx_vpu_buf *buf;
	struct vpu_dec_ctx *dec_ctx = &ctx->codec.dec;
	int i, j;

	for(i = 0; i < dec_ctx->frame_buffer_cnt; ++i) {
		if( (buf = dec_ctx->dpb_bufs[i]) != NULL ) {
			for( j = 0; j < buf->vb.num_planes; ++j )
				vb2_set_plane_payload(&buf->vb, j, 0);
			vb2_buffer_done(&buf->vb, state);
			dec_ctx->dpb_bufs[i] = NULL;
		}
	}
	dec_ctx->dpb_queue_cnt = 0;
}

static int nx_vpu_dec_start_streaming(struct vb2_queue *q, unsigned int count)
{
	unsigned long flags;
	struct nx_vpu_ctx *ctx = q->drv_priv;
	int ret = 0;

	FUNC_IN();

	if( q->type == ctx->vq_strm.type ) {
		ret = nx_vpu_dec_try_cmd(ctx, GET_DEC_INSTANCE);
		if( ret ) {
			spin_lock_irqsave(&ctx->dev->irqlock, flags);
			nx_vpu_cleanup_queue(&ctx->strm_queue, &ctx->vq_strm,
					VB2_BUF_STATE_QUEUED);
			INIT_LIST_HEAD(&ctx->strm_queue);
			ctx->strm_queue_cnt = 0;
			spin_unlock_irqrestore(&ctx->dev->irqlock, flags);
		}else{
		   	while( nx_vpu_dec_ctx_ready(ctx) )
				nx_vpu_dec_try_cmd(ctx, DEC_RUN);
		}
	}else if( q->type == ctx->vq_img.type ) {
		while ( nx_vpu_dec_ctx_ready(ctx) )
			ret = nx_vpu_dec_try_cmd(ctx, DEC_RUN);
		if( ret ) {
			spin_lock_irqsave(&ctx->dev->irqlock, flags);
			cleanup_dpb_queue(ctx, VB2_BUF_STATE_QUEUED);
			spin_unlock_irqrestore(&ctx->dev->irqlock, flags);
		}
	}
	return ret;
}

static void nx_vpu_dec_stop_streaming(struct vb2_queue *q)
{
	unsigned long flags;
	struct nx_vpu_ctx *ctx = q->drv_priv;
	struct nx_vpu_v4l2 *dev = ctx->dev;

	FUNC_IN();

	nx_vpu_dec_try_cmd(ctx, SEQ_END);
	if (q->type == ctx->vq_img.type ) {
		spin_lock_irqsave(&dev->irqlock, flags);
		cleanup_dpb_queue(ctx, VB2_BUF_STATE_ERROR);
		spin_unlock_irqrestore(&dev->irqlock, flags);
	} else if ( q->type == ctx->vq_strm.type ) {
		spin_lock_irqsave(&dev->irqlock, flags);

		nx_vpu_cleanup_queue(&ctx->strm_queue, &ctx->vq_strm,
				VB2_BUF_STATE_ERROR);
		INIT_LIST_HEAD(&ctx->strm_queue);
		ctx->strm_queue_cnt = 0;

		spin_unlock_irqrestore(&dev->irqlock, flags);
	}
}

static void nx_vpu_dec_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct nx_vpu_ctx *ctx = vq->drv_priv;
	struct nx_vpu_v4l2 *dev = ctx->dev;
	unsigned long flags;
	struct nx_vpu_buf *buf = vb_to_vpu_buf(vb);

	FUNC_IN();

	spin_lock_irqsave(&dev->irqlock, flags);

	if ( vq->type == ctx->vq_strm.type ) {
		list_add_tail(&buf->list, &ctx->strm_queue);
		ctx->strm_queue_cnt++;
	} else if (vq->type == ctx->vq_img.type ) {
		struct vpu_dec_ctx *dec_ctx = &ctx->codec.dec;
		unsigned idx;
		int num_planes = ctx->useSingleBuf || ctx->img_fmt->singleBuffer ? 1 :
			ctx->img_fmt->chromaInterleave ? 2 : 3;
		uint32_t phyAddr0 = nx_vpu_mem_plane_addr(vb, 0);

		/* Match buffer by their memory physical address.
		 * For given buffer index their memory address may change,
		 * especially for imported DMA buffers */
		for(idx = 0; idx < dec_ctx->frame_buffer_cnt; ++idx) {
			if( dec_ctx->phyAddrs.addr[idx][0] == phyAddr0 )
				break;
		}
		if( idx == dec_ctx->frame_buffer_cnt ) {
			if( dec_ctx->frame_buffer_cnt == VPU_MAX_BUFFERS ) {
				/* limit reached: old buffers removal not implemented for now
				 */
				WARN_ONCE(1, "nxp-vpu: buffer limit reached");
				vb2_buffer_done(&buf->vb, VB2_BUF_STATE_ERROR);
				return;
			}
			dec_ctx->phyAddrs.addr[idx][0] = phyAddr0;
			if( num_planes > 1 ) {
				dec_ctx->phyAddrs.addr[idx][1] = nx_vpu_mem_plane_addr(vb, 1);
			} else if (ctx->chroma_size > 0) {
				dec_ctx->phyAddrs.addr[idx][1] = ctx->luma_size +
					dec_ctx->phyAddrs.addr[idx][0];
			}

			if( num_planes > 2 ) {
				dec_ctx->phyAddrs.addr[idx][2] = nx_vpu_mem_plane_addr(vb, 2);
			} else if (ctx->chroma_size > 0 && ctx->chromaInterleave == 0) {
				dec_ctx->phyAddrs.addr[idx][2] = ctx->chroma_size +
					dec_ctx->phyAddrs.addr[idx][1];
			}
			++dec_ctx->frame_buffer_cnt;
		}
		if( dec_ctx->dpb_bufs[idx] ) {
			NX_ErrMsg(("buffer %d has the same payload address as buffer %d\n",
						buf->vb.index, dec_ctx->dpb_bufs[idx]->vb.index));
			vb2_buffer_done(&buf->vb, VB2_BUF_STATE_ERROR);
			return;
		}
		dec_ctx->dpb_bufs[idx] = buf;
		dec_ctx->dpb_queue_cnt++;
		if( ctx->codec.dec.state != NX_VPUDEC_CLOSED ) {
			int ret = NX_VpuDecClrDspFlag(ctx->hInst, idx);
			if (ret != VPU_RET_OK) {
				NX_ErrMsg(("ClrDspFlag error %d\n", ret));
			}
		}
	} else {
		NX_ErrMsg(("unsupported buffer type(%d)\n", vq->type));
		return;
	}

	spin_unlock_irqrestore(&dev->irqlock, flags);

	if (nx_vpu_dec_ctx_ready(ctx))
		nx_vpu_dec_try_cmd(ctx, DEC_RUN);
}

static struct vb2_ops nx_vpu_dec_qops = {
	.queue_setup            = nx_vpu_queue_setup,
	.wait_prepare           = nx_vpu_unlock,
	.wait_finish            = nx_vpu_lock,
	.buf_prepare            = nx_vpu_buf_prepare,
	.start_streaming        = nx_vpu_dec_start_streaming,
	.stop_streaming         = nx_vpu_dec_stop_streaming,
	.buf_queue              = nx_vpu_dec_buf_queue,
};

/* -------------------------------------------------------------------------- */


const struct v4l2_ioctl_ops *get_dec_ioctl_ops(bool singlePlaneMode)
{
	return singlePlaneMode ? &nx_vpu_dec_ioctl_ops :
		&nx_vpu_dec_ioctl_ops_mplane;
}

int nx_vpu_dec_open(struct nx_vpu_ctx *ctx, bool singlePlaneMode)
{
	int ret = 0;

	FUNC_IN();

	ctx->codec.dec.dpb_queue_cnt = 0;

	/* Init videobuf2 queue for OUTPUT */
	ctx->vq_strm.type = singlePlaneMode ? V4L2_BUF_TYPE_VIDEO_OUTPUT :
		V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ctx->vq_strm.drv_priv = ctx;
	ctx->vq_strm.lock = &ctx->dev->dev_mutex;
	ctx->vq_strm.buf_struct_size = sizeof(struct nx_vpu_buf);
	ctx->vq_strm.io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	ctx->vq_strm.mem_ops = &vb2_dma_contig_memops;
	/*ctx->vq_strm.allow_zero_byteused = 1; */
	ctx->vq_strm.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	ctx->vq_strm.ops = &nx_vpu_dec_qops;
	ctx->vq_strm.dev = &ctx->dev->plat_dev->dev;
	ret = vb2_queue_init(&ctx->vq_strm);
	if (ret) {
		NX_ErrMsg(("Failed to initialize videobuf2 queue(output)\n"));
		return ret;
	}

	/* Init videobuf2 queue for CAPTURE */
	ctx->vq_img.type = singlePlaneMode ? V4L2_BUF_TYPE_VIDEO_CAPTURE :
		V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ctx->vq_img.drv_priv = ctx;
	ctx->vq_img.lock = &ctx->dev->dev_mutex;
	ctx->vq_img.buf_struct_size = sizeof(struct nx_vpu_buf);
	ctx->vq_img.io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	ctx->vq_img.mem_ops = &vb2_dma_contig_memops;
	ctx->vq_img.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	ctx->vq_img.ops = &nx_vpu_dec_qops;
	ctx->vq_img.dev = &ctx->dev->plat_dev->dev;
	ret = vb2_queue_init(&ctx->vq_img);
	if (ret) {
		NX_ErrMsg(("Failed to initialize videobuf2 queue(capture)\n"));
		return ret;
	}

	return 0;
}

static void decoder_flush_disp_info(struct vpu_dec_ctx *dec_ctx)
{
	int32_t i;

	for (i = 0 ; i < VPU_MAX_BUFFERS ; i++) {
		dec_ctx->timeStamp[i] = 0;
		dec_ctx->frm_type[i] = -1;
		dec_ctx->multiResolution[i] = -0;
		dec_ctx->interlace_flg[i] = -1;
		dec_ctx->reliable_0_100[i] = 0;
		dec_ctx->upSampledWidth[i] = 0;
		dec_ctx->upSampledHeight[i] = 0;
	}
}

int vpu_dec_open_instance(struct nx_vpu_ctx *ctx)
{
	struct nx_vpu_v4l2 *dev = ctx->dev;
	struct vpu_dec_ctx *dec_ctx = &ctx->codec.dec;
	struct nx_vpu_codec_inst *hInst = 0;
	struct vpu_open_arg openArg;
	int workBufSize = WORK_BUF_SIZE;
	int ret = 0;

	FUNC_IN();

	memset(&openArg, 0, sizeof(openArg));

	switch (ctx->strm_fmt->fourcc) {
	case V4L2_PIX_FMT_H264:
		ctx->codec_mode = CODEC_STD_AVC;
		workBufSize += PS_SAVE_SIZE;
		break;
	case V4L2_PIX_FMT_MPEG2:
		ctx->codec_mode = CODEC_STD_MPEG2;
		break;
	case V4L2_PIX_FMT_MPEG4:
		ctx->codec_mode = CODEC_STD_MPEG4;
		break;
	case V4L2_PIX_FMT_XVID:
		ctx->codec_mode = CODEC_STD_MPEG4;
		openArg.mp4Class = 2;
		break;
	case V4L2_PIX_FMT_DIV4:
	case V4L2_PIX_FMT_DIVX:
		ctx->codec_mode = CODEC_STD_MPEG4;
		openArg.mp4Class = 5;
		break;
	case V4L2_PIX_FMT_DIV5:
	case V4L2_PIX_FMT_DIV6:
		ctx->codec_mode = CODEC_STD_MPEG4;
		openArg.mp4Class = 1;
		break;
	case V4L2_PIX_FMT_H263:
		ctx->codec_mode = CODEC_STD_H263;
		break;
	case V4L2_PIX_FMT_DIV3:
		ctx->codec_mode = CODEC_STD_DIV3;
		break;
	case V4L2_PIX_FMT_WMV9:
	case V4L2_PIX_FMT_WVC1:
		ctx->codec_mode = CODEC_STD_VC1;
		break;
	case V4L2_PIX_FMT_RV8:
	case V4L2_PIX_FMT_RV9:
		ctx->codec_mode = CODEC_STD_RV;
		break;
	case V4L2_PIX_FMT_FLV1:
		/* Sorenson spark */
		ctx->codec_mode = CODEC_STD_MPEG4;
		openArg.mp4Class = 256;
		break;
	case V4L2_PIX_FMT_THEORA:
		ctx->codec_mode = CODEC_STD_THO;
		break;
	case V4L2_PIX_FMT_VP8:
		ctx->codec_mode = CODEC_STD_VP8;
		break;
	case V4L2_PIX_FMT_MJPEG:
		ctx->codec_mode = CODEC_STD_MJPG;
		break;
	default:
		NX_ErrMsg(("Invalid codec type(fourcc = %x)!!!\n",
			ctx->strm_fmt->fourcc));
		goto err_exit;
	}
	openArg.codecStd = ctx->codec_mode;

	ctx->bit_stream_buf = nx_alloc_memory(&dev->plat_dev->dev,
		STREAM_BUF_SIZE, 4096);
	if (ctx->bit_stream_buf == NULL) {
		NX_ErrMsg(("Bitstream_buf allocation failed.\n"));
		goto err_exit;
	}

	ctx->instance_buf = nx_alloc_memory(&dev->plat_dev->dev,
		workBufSize, 4096);
	if (ctx->instance_buf == NULL) {
		NX_ErrMsg(("instance_buf allocation failed.\n"));
		goto err_exit;
	}

	if (ctx->codec_mode == CODEC_STD_AVC) {
		dec_ctx->slice_buf = nx_alloc_memory(&dev->plat_dev->dev,
				2048 * 2048 * 3 / 4, 4096);
		if (0 == dec_ctx->slice_buf) {
			NX_ErrMsg(("slice buf allocation failed(size = %d)\n",
				2048 * 2048 * 3 / 4));
			goto err_exit;
		}
	}

	if (ctx->codec_mode == CODEC_STD_THO || ctx->codec_mode == CODEC_STD_VP3
		|| ctx->codec_mode == CODEC_STD_VP8) {
			dec_ctx->pv_slice_buf = nx_alloc_memory(&dev->plat_dev->dev,
					17 * 4 * (2048 * 2048 / 256), 4096);
		if (0 == dec_ctx->pv_slice_buf) {
			NX_ErrMsg(("slice allocation failed(size=%d)\n",
				17 * 4 * (2048 * 2048 / 256)));
			goto err_exit;
		}
	}

	openArg.instIndex = ctx->idx;
	openArg.instanceBuf = *ctx->instance_buf;
	openArg.streamBuf = *ctx->bit_stream_buf;

	ret = NX_VpuDecOpen(&openArg, dev, &hInst);
	if ((VPU_RET_OK != ret) || (0 == hInst)) {
		NX_ErrMsg(("Cannot open VPU Instance!!\n"));
		NX_ErrMsg(("  codecStd=%d, is_encoder=%d, hInst=%p)\n",
			openArg.codecStd, openArg.isEncoder, hInst));
		goto err_exit;
	}

	decoder_flush_disp_info(&ctx->codec.dec);

	ctx->hInst = (void *)hInst;
	dev->cur_num_instance++;

	return ret;

err_exit:
	if (dec_ctx->pv_slice_buf) {
		nx_free_memory(dec_ctx->pv_slice_buf);
		dec_ctx->pv_slice_buf = NULL;
	}
	if (dec_ctx->slice_buf) {
		nx_free_memory(dec_ctx->slice_buf);
		dec_ctx->slice_buf = NULL;
	}
	if (ctx->instance_buf) {
		nx_free_memory(ctx->instance_buf);
		ctx->instance_buf = NULL;
	}
	if (ctx->bit_stream_buf) {
		nx_free_memory(ctx->bit_stream_buf);
		ctx->bit_stream_buf = NULL;
	}
	return ret;
}

int vpu_dec_parse_vid_cfg(struct nx_vpu_ctx *ctx, bool singlePlaneMode)
{
	int ret;
	struct vpu_dec_ctx *dec_ctx = &ctx->codec.dec;
	struct vpu_dec_seq_init_arg seqArg;
	struct nx_vpu_buf *buf;
	struct vb2_v4l2_buffer *vbuf;
	unsigned long flags;
	unsigned fourcc;
	const struct nx_vpu_image_fmt *img_fmt;

	FUNC_IN();

	if (ctx->hInst == NULL) {
		NX_ErrMsg(("Err : vpu is not opend\n"));
		return -EAGAIN;
	}

	NX_DrvMemset(&seqArg, 0, sizeof(seqArg));

	if (ctx->strm_queue_cnt > 0) {
		unsigned long phyAddr;

		/*spin_lock_irqsave(&ctx->dev->irqlock, flags);*/

		if (list_empty(&ctx->strm_queue)) {
			NX_DbgMsg(INFO_MSG, ("No src buffer.\n"));
			/* spin_unlock_irqrestore(&ctx->dev->irqlock, flags); */
			return -EAGAIN;
		}

		buf = list_entry(ctx->strm_queue.next, struct nx_vpu_buf, list);
		phyAddr = nx_vpu_mem_plane_addr(&buf->vb, 0);
		seqArg.seqDataSize = buf->vb.planes[0].bytesused;

#ifdef USE_ION_MEMORY
		{
			int alignSz;

			alignSz = (seqArg.seqDataSize + 4095) & (~4095);
			seqArg.seqData = (unsigned char *)cma_get_virt(phyAddr,
				alignSz, 1);
		}
#else
		seqArg.seqData = (unsigned long)vb2_plane_vaddr(&buf->vb, 0);
#endif

		/*spin_unlock_irqrestore(&ctx->dev->irqlock, flags);*/
	} else {
		return -EAGAIN;
	}

	seqArg.outWidth = ctx->width;
	seqArg.outHeight = ctx->height;

	seqArg.thumbnailMode = dec_ctx->thumbnailMode;

	ret = NX_VpuDecSetSeqInfo(ctx->hInst, &seqArg);
	if (ret != VPU_RET_OK) {
		NX_ErrMsg(("NX_VpuDecSetSeqInfo() failed.(ErrorCode=%d)\n",
			ret));
		return -EINVAL;
	}

	if (seqArg.minFrameBufCnt < 1 ||
		seqArg.minFrameBufCnt > VPU_MAX_BUFFERS)
	{
		NX_ErrMsg(("Min FrameBufCnt Error(%d)!!!\n",
			seqArg.minFrameBufCnt));
		return -EINVAL;
	}

	ctx->width = seqArg.cropRight;
	ctx->height = seqArg.cropBottom;
	dec_ctx->minFrameBufCnt = seqArg.minFrameBufCnt;

	dec_ctx->interlace_flg[0] = (seqArg.interlace == 0) ?
		(V4L2_FIELD_NONE) : (V4L2_FIELD_INTERLACED);
	dec_ctx->frame_buf_delay = seqArg.frameBufDelay;
	ctx->buf_y_width = ALIGN(ctx->width, 8);
	ctx->buf_height = ctx->height;
	ctx->luma_size = ctx->buf_y_width * ctx->buf_height;

	switch (seqArg.imgFormat) {
	case IMG_FORMAT_420:
		fourcc = singlePlaneMode ? V4L2_PIX_FMT_YUV420 : V4L2_PIX_FMT_YUV420M;
		break;
	case IMG_FORMAT_422:
		fourcc = singlePlaneMode ? V4L2_PIX_FMT_YUV422P : V4L2_PIX_FMT_YUV422M;
		break;
	case IMG_FORMAT_444:
		fourcc = singlePlaneMode ? V4L2_PIX_FMT_YUV444 : V4L2_PIX_FMT_YUV444M;
		break;
	case IMG_FORMAT_400:
		fourcc = V4L2_PIX_FMT_GREY;
		break;
	default:
		NX_ErrMsg(("Image format is not supported!!\n"));
		return -EINVAL;
	}
	img_fmt = nx_find_image_format(fourcc);
	if( img_fmt == NULL ) {
		NX_ErrMsg(("internal error: format %.4s not found\n", (char*)&fourcc));
		return -EINVAL;
	}
	if( singlePlaneMode && ! img_fmt->singleBuffer ) {
		NX_ErrMsg(("internal error: format %.4s is multi-plane\n",
					(char*)&fourcc));
		return -EINVAL;
	}
	ctx->img_fmt = img_fmt;
	if( img_fmt->hsub ) {
		ctx->buf_c_width = ctx->buf_y_width / img_fmt->hsub;
		ctx->chroma_size = ctx->buf_c_width * ctx->buf_height / img_fmt->vsub;
	}else{
		ctx->buf_c_width = 0;
		ctx->chroma_size = 0;
	}
	ctx->chromaInterleave = img_fmt->chromaInterleave;

	dec_ctx->start_Addr = 0;
	dec_ctx->end_Addr = seqArg.strmReadPos;
	ctx->strm_size = dec_ctx->end_Addr - dec_ctx->start_Addr;

	dec_ctx->crop_left = seqArg.cropLeft;
	dec_ctx->crop_right = seqArg.cropRight;
	dec_ctx->crop_top = seqArg.cropTop;
	dec_ctx->crop_bot = seqArg.cropBottom;

	NX_DbgMsg(INFO_MSG, ("[PARSE]Min_Buff = %d\n",
		dec_ctx->minFrameBufCnt));

	spin_lock_irqsave(&ctx->dev->irqlock, flags);

	buf = list_entry(ctx->strm_queue.next, struct nx_vpu_buf, list);
	vbuf = to_vb2_v4l2_buffer(&buf->vb);
	list_del(&buf->list);
	ctx->strm_queue_cnt--;

	buf->vb.planes[0].bytesused = ctx->strm_size;
	vbuf->field = dec_ctx->interlace_flg[0];
	vbuf->flags = ctx->codec.dec.frame_buf_delay;

	vb2_buffer_done(&buf->vb, VB2_BUF_STATE_DONE);

	spin_unlock_irqrestore(&ctx->dev->irqlock, flags);

	return ret;
}

static struct nx_memory_info *alloc_mvbuf(struct nx_vpu_ctx *ctx)
{
	struct vpu_dec_ctx *dec_ctx = &ctx->codec.dec;
	int mvSize;
	void *drv = &ctx->dev->plat_dev->dev;

	mvSize = ALIGN(ctx->width, 32) * ALIGN(ctx->height, 32);
	mvSize = (mvSize * 3) / 2;
	mvSize = (mvSize + 4) / 5;
	mvSize = ((mvSize + 7) / 8) * 8;
	mvSize = ALIGN(mvSize, 4096);

	if ( mvSize == 0 || dec_ctx->frame_buffer_cnt == 0 ) {
		NX_ErrMsg(("Invalid memory parameters!!!\n"));
		NX_ErrMsg(("width=%d, height=%d, mvSize=%d buffer_cnt=%d\n",
				ctx->width, ctx->height, mvSize, dec_ctx->frame_buffer_cnt));
		return NULL;
	}
	return nx_alloc_memory(drv, mvSize * dec_ctx->frame_buffer_cnt, 4096);
}

int vpu_dec_init(struct nx_vpu_ctx *ctx)
{
	struct vpu_dec_ctx *dec_ctx = &ctx->codec.dec;
	struct vpu_dec_reg_frame_arg frameArg;
	int ret = 0;
	struct nx_memory_info *mvbuf = NULL; // new move buf to replace col_mv_buf

	FUNC_IN();

	if (ctx->hInst == NULL) {
		NX_ErrMsg(("Err : vpu is not opend\n"));
		return -EAGAIN;
	}
	frameArg.chromaInterleave = ctx->chromaInterleave;

	if (ctx->codec_mode != CODEC_STD_MJPG) {
		mvbuf = alloc_mvbuf(ctx);
		if( mvbuf == NULL ) {
			NX_ErrMsg(("Failed to allocate decoder buffers.\n"));
			return -ENOMEM;
		}

		if (dec_ctx->slice_buf)
			frameArg.sliceBuffer = dec_ctx->slice_buf;
		frameArg.colMvBuffer = mvbuf;
		if (dec_ctx->pv_slice_buf)
			frameArg.pvbSliceBuffer = dec_ctx->pv_slice_buf;

		frameArg.sramAddr = ctx->dev->sram_base_addr;
		frameArg.sramSize = ctx->dev->sram_size;
	}
	frameArg.numFrameBuffer = dec_ctx->frame_buffer_cnt;
	frameArg.strideY =  ctx->buf_y_width;
	frameArg.phyAddrs = &dec_ctx->phyAddrs;

	ret = NX_VpuDecRegFrameBuf(ctx->hInst, &frameArg);
	if (ret == VPU_RET_OK) {
		if( dec_ctx->col_mv_buf )
			nx_free_memory(dec_ctx->col_mv_buf);
		dec_ctx->col_mv_buf = mvbuf;
		dec_ctx->registeredCount = dec_ctx->frame_buffer_cnt;
	}else{
		NX_ErrMsg(("NX_VpuDecRegFrameBuf() failed.(ErrorCode=%d)\n", ret));
		if( mvbuf )
			nx_free_memory(mvbuf);
	}

	return ret;
}

static void put_dec_info(struct nx_vpu_ctx *ctx,
	struct vpu_dec_frame_arg *pDecArg, const u64 timestamp)
{
	struct vpu_dec_ctx *dec_ctx = &ctx->codec.dec;
	int32_t idx = pDecArg->indexFrameDecoded;

	if (idx < 0)
		return;

	if ((pDecArg->isInterace) || ((ctx->codec_mode == CODEC_STD_MPEG2) &&
		(pDecArg->picStructure != 3)))
		dec_ctx->interlace_flg[idx] = (pDecArg->topFieldFirst) ?
			(V4L2_FIELD_SEQ_TB) : (V4L2_FIELD_SEQ_BT);
	else
		dec_ctx->interlace_flg[idx] = V4L2_FIELD_NONE;

	switch (pDecArg->picType) {
	case 0:
	case 6:
		dec_ctx->frm_type[idx] = V4L2_BUF_FLAG_KEYFRAME;
		break;
	case 1:
	case 4:
	case 5:
		dec_ctx->frm_type[idx] = V4L2_BUF_FLAG_PFRAME;
		break;
	case 2:
	case 3:
		dec_ctx->frm_type[idx] = V4L2_BUF_FLAG_BFRAME;
		break;
	case 7:
		dec_ctx->frm_type[idx] = -1;
		break;
	default:
		NX_ErrMsg(("not defined frame type!!!\n"));
		dec_ctx->frm_type[idx] = -1;
	}

	if (pDecArg->numOfErrMBs == 0) {
		dec_ctx->cur_reliable = 100;
	} else {
		if (ctx->codec_mode != CODEC_STD_MJPG) {
			int totalMbNum = ((pDecArg->outWidth + 15) >> 4) *
				((pDecArg->outHeight + 15) >> 4);
			dec_ctx->cur_reliable = (totalMbNum -
				pDecArg->numOfErrMBs) * 100 / totalMbNum;
		} else {
			int32_t PosX = ((pDecArg->numOfErrMBs >> 12) & 0xFFF) *
				pDecArg->mcuWidth;
			int32_t PosY = (pDecArg->numOfErrMBs & 0xFFF) *
				pDecArg->mcuHeight;
			int32_t PosRst = ((pDecArg->numOfErrMBs >> 24) & 0xF) *
				pDecArg->mcuWidth * pDecArg->mcuHeight;
			dec_ctx->cur_reliable = (PosRst + (PosY *
				pDecArg->outWidth) + PosX) * 100 /
				(pDecArg->outWidth * pDecArg->outHeight);
		}
	}

	if ((dec_ctx->interlace_flg[idx] == V4L2_FIELD_NONE) ||
		(pDecArg->npf))
		dec_ctx->reliable_0_100[idx] = dec_ctx->cur_reliable;
	else
		dec_ctx->reliable_0_100[idx] += dec_ctx->cur_reliable >> 1;

	dec_ctx->timeStamp[idx] = timestamp;
}

int vpu_dec_decode_slice(struct nx_vpu_ctx *ctx, bool flush)
{
	struct vpu_dec_ctx *dec_ctx = &ctx->codec.dec;
	int ret = 0;
	unsigned long flags;
	u64 timestamp;
	struct vpu_dec_frame_arg decArg;
	struct nx_vpu_v4l2 *dev = ctx->dev;
	struct nx_vpu_buf *strmBuf, *doneBuf = NULL;
	struct vb2_v4l2_buffer *vbuf;
	void *strmData;
	unsigned strmDataSize;


	if (ctx->hInst == NULL) {
		NX_ErrMsg(("Err : vpu is not opend\n"));
		return -EAGAIN;
	}

	if( dec_ctx->frame_buffer_cnt > dec_ctx->registeredCount ) {
		ret = vpu_dec_init(ctx);
		if( ret ) {
			NX_ErrMsg(("vpu_dec_decode_slice: additional buffers register "
						"failed\n"));
			return ret;
		}
	}

	NX_DrvMemset(&decArg, 0, sizeof(decArg));

	if ( !flush ) {
		int alignSz;
		unsigned long phyAddr;

		/* spin_lock_irqsave(&dev->irqlock, flags); */

		if (list_empty(&ctx->strm_queue)) {
			NX_ErrMsg(("No src buffer.\n"));
			/* spin_unlock_irqrestore(&ctx->dev->irqlock, flags); */
			return -EAGAIN;
		}

		strmBuf = list_entry(ctx->strm_queue.next, struct nx_vpu_buf, list);
		vbuf = to_vb2_v4l2_buffer(&strmBuf->vb);
		phyAddr = nx_vpu_mem_plane_addr(&strmBuf->vb, 0);
		strmDataSize = vb2_get_plane_payload(&strmBuf->vb, 0);
		alignSz = (strmDataSize + 4095) & (~4095);
#ifdef USE_ION_MEMORY
		strmData = cma_get_virt(phyAddr, alignSz, 1);
#else
		strmData = vb2_plane_vaddr(&strmBuf->vb, 0);
#endif

		decArg.eos = 0;
		timestamp = vbuf->vb2_buf.timestamp;

		/* spin_unlock_irqrestore(&dev->irqlock, flags); */

		if( strmDataSize ) {
			ret = NX_VpuFillStreamBuffer(ctx->hInst, strmData, strmDataSize);
			if( ret != VPU_RET_OK ) {
				NX_ErrMsg(("NX_VpuFillStreamBuffer() failed. (ErrorCode=%d)\n", ret));
				return ret;
			}
		}
		list_del(&strmBuf->list);
		ctx->strm_queue_cnt--;
		vb2_buffer_done(&strmBuf->vb, VB2_BUF_STATE_DONE);
	} else {
		decArg.eos = 1;
		timestamp = 0;
	}

	dec_ctx->start_Addr = dec_ctx->end_Addr;

	ret = NX_VpuDecRunFrame(ctx->hInst, &decArg);
	if (ret != VPU_RET_OK) {
		NX_ErrMsg(("NX_VpuDecRunFrame() failed.(ErrorCode=%d)\n", ret));
		return ret;
	}
	if( flush ) {
		int ret = NX_VpuDecFlush(ctx->hInst);
		if( ret )
			NX_ErrMsg(("NX_VpuDecFlush err=%d\n", ret));
	}

	dec_ctx->end_Addr = decArg.strmReadPos;

	if (dec_ctx->end_Addr >= dec_ctx->start_Addr)
		ctx->strm_size = dec_ctx->end_Addr - dec_ctx->start_Addr;
	else
		ctx->strm_size = (STREAM_BUF_SIZE - dec_ctx->start_Addr)
				+ dec_ctx->end_Addr;

	put_dec_info(ctx, &decArg, timestamp);

	dec_ctx->delay_frm = -1;
	if (decArg.indexFrameDisplay >= 0 &&
			decArg.indexFrameDisplay < dec_ctx->frame_buffer_cnt)
	{

		spin_lock_irqsave(&dev->irqlock, flags);

		doneBuf = dec_ctx->dpb_bufs[decArg.indexFrameDisplay];
		if ( doneBuf ) {
			int i;
			vbuf = to_vb2_v4l2_buffer(&doneBuf->vb);

			vbuf->field = decArg.isInterace;
			vbuf->flags = decArg.picType;
			for (i = 0; i < doneBuf->vb.num_planes; i++)
				vb2_set_plane_payload(&doneBuf->vb, i,
						vb2_plane_size(&doneBuf->vb, i));
			dec_ctx->dpb_bufs[decArg.indexFrameDisplay] = NULL;
			dec_ctx->dpb_queue_cnt--;
			dec_ctx->delay_frm = 0;
		}else{
			NX_ErrMsg(("decoder returned frame %d not associated with a buffer\n",
					decArg.indexFrameDisplay));
		}

		spin_unlock_irqrestore(&dev->irqlock, flags);
	}else if (decArg.indexFrameDisplay >= 0 ) {
		NX_ErrMsg(("bad display frame number returned by decoder: %d\n",
					decArg.indexFrameDisplay));
	} else if (decArg.indexFrameDisplay == -3) {
		NX_DbgMsg(INFO_MSG, ("delayed Output(%d)\n",
			decArg.indexFrameDisplay));
		dec_ctx->delay_frm = 1;
	} else if (decArg.indexFrameDisplay == -2) {
		NX_DbgMsg(INFO_MSG, ("Skip Frame\n"));
	} else if( !flush ) {
		NX_ErrMsg(("There is not decoded img!!! (idx=%d)\n",
					decArg.indexFrameDisplay));
	}

	/*spin_lock_irqsave(&dev->irqlock, flags);*/

	if( doneBuf ) {
		int idx = decArg.indexFrameDisplay;

		vbuf = to_vb2_v4l2_buffer(&doneBuf->vb);
		vbuf->field = dec_ctx->interlace_flg[idx];
		vbuf->flags = dec_ctx->frm_type[idx];
		vbuf->vb2_buf.timestamp = dec_ctx->timeStamp[idx];
		if( flush )
			vbuf->flags |= V4L2_BUF_FLAG_LAST;
		vb2_buffer_done(&doneBuf->vb, VB2_BUF_STATE_DONE);
	}else if( flush ) {
		// report end of stream using empty buffer
		int i, idx = 0;
		while( idx < dec_ctx->frame_buffer_cnt &&
				dec_ctx->dpb_bufs[idx] == NULL)
			++idx;
		if( idx < dec_ctx->frame_buffer_cnt ) {
			doneBuf = dec_ctx->dpb_bufs[idx];
			dec_ctx->dpb_bufs[idx] = NULL;
			vbuf = to_vb2_v4l2_buffer(&doneBuf->vb);
			vbuf->flags = V4L2_BUF_FLAG_LAST;
			for(i = 0; i < doneBuf->vb.num_planes; i++)
				vb2_set_plane_payload(&doneBuf->vb, i, 0);
			vb2_buffer_done(&doneBuf->vb, VB2_BUF_STATE_DONE);
		}
	}

	/*spin_unlock_irqrestore(&dev->irqlock, flags);*/

	return ret;
}

static int free_decoder_memory(struct nx_vpu_ctx *ctx)
{
	struct vpu_dec_ctx *dec_ctx = &ctx->codec.dec;

	FUNC_IN();

	if (!ctx) {
		NX_ErrMsg(("invalid decoder handle!!!\n"));
		return -1;
	}

	if (dec_ctx->col_mv_buf) {
		nx_free_memory(dec_ctx->col_mv_buf);
		dec_ctx->col_mv_buf = NULL;
	}

	if (dec_ctx->slice_buf) {
		nx_free_memory(dec_ctx->slice_buf);
		dec_ctx->slice_buf = NULL;
	}

	if (dec_ctx->pv_slice_buf) {
		nx_free_memory(dec_ctx->pv_slice_buf);
		dec_ctx->pv_slice_buf = NULL;
	}

	if (ctx->instance_buf) {
		nx_free_memory(ctx->instance_buf);
		ctx->instance_buf = NULL;
	}

	if (ctx->bit_stream_buf) {
		nx_free_memory(ctx->bit_stream_buf);
		ctx->bit_stream_buf = NULL;
	}

	return 0;
}

void nx_vpu_dec_close_instance(struct nx_vpu_ctx *ctx)
{
	struct nx_vpu_v4l2 *dev = ctx->dev;

	if (ctx->hInst) {
		int ret = NX_VpuDecClose(ctx->hInst, (void*)&dev->vpu_event_present);
		if (ret != 0)
			NX_ErrMsg(("Failed to return an instance.\n"));
		free_decoder_memory(ctx);
		--dev->cur_num_instance;
		ctx->hInst = NULL;
	}
}

