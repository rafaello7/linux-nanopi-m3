#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>
#include "s5pxx18_dp_dev.h"


static bool single_plane_mode;
module_param(single_plane_mode, bool, 0444);


/* Pixel format description table.
 * The video layer supports only YUV formats (Y - luminance, U - red value,
 * V - blue). There is only one non-planar format, YUYV. Remaining formats are
 * planar. Three planes may be in one memory chunk or in three. Luminance plane
 * is first one. Red plane may be before blue plane (YUV) or after (YVU).
 * Single color value may be for every pixel (YUV444/YVU444), for two adjacent
 * horizontal pixels (YUV422/YVU422) or for 2x2 pixel square (YUV420/YVU420).
 */
static const struct NanoVideoFormat {
	uint32_t fourcc;
	enum nx_mlc_yuvfmt nxfmt;
	bool isVU;				// for planar format: YUV = false, YVU = true
	unsigned num_planes;
	unsigned bpp;			// bytes per pixel (in planar format: of Y plane)
    unsigned hsub, vsub;	// horizontal/vertical subsample
} supported_video_formats[] = {
	/* 1-buffer */
	{
		V4L2_PIX_FMT_YUV420,  nx_mlc_yuvfmt_420,  false, 1, 1, 2, 2
	},{
		V4L2_PIX_FMT_YVU420,  nx_mlc_yuvfmt_420,  true,  1, 1, 2, 2
	},{
		V4L2_PIX_FMT_YUV422P, nx_mlc_yuvfmt_422,  false, 1, 1, 2, 1
	},{
		V4L2_PIX_FMT_YUV444,  nx_mlc_yuvfmt_444,  false, 1, 1, 1, 1
	},{
		V4L2_PIX_FMT_YUYV,	  nx_mlc_yuvfmt_yuyv, false, 1, 2, 0, 0
	},
	/* 3-buffer */
	{
		V4L2_PIX_FMT_YUV420M, nx_mlc_yuvfmt_420,  false, 3, 1, 2, 2
	},{
		V4L2_PIX_FMT_YVU420M, nx_mlc_yuvfmt_420,  true,  3, 1, 2, 2
	},{
		V4L2_PIX_FMT_YUV422M, nx_mlc_yuvfmt_422,  false, 3, 1, 2, 1
	},{
		V4L2_PIX_FMT_YVU422M, nx_mlc_yuvfmt_422,  true,  3, 1, 2, 1
	},{
		V4L2_PIX_FMT_YUV444M, nx_mlc_yuvfmt_444,  false, 3, 1, 1, 1
	},{
		V4L2_PIX_FMT_YVU444M, nx_mlc_yuvfmt_444,  true,  3, 1, 1, 1
	}
};

struct nx_videolayer {
	int module;
	bool isEnabled;
	bool isVidiocOverlayOn;

	/* buffer wanted to display now (if displaying is enabled) */
	struct vb2_buffer *bufWantDisplay;

	/* Buffers whose (may be) currently displayed.
	 * Buffer deactivation occurs asynchronously at vsync signal.
	 */
	struct vb2_buffer *bufsActiveTillDirty[2];

	spinlock_t wq_lock;
	bool isWorkActive;
	struct workqueue_struct *workqueue;
	struct work_struct buf_switch_wait_work;

	/* source */
	const struct NanoVideoFormat *format;
	int width;
	int height;
	bool useSingleBuf;

	/* target */
	int dst_left;
	int dst_top;
	int dst_width;
	int dst_height;

	/* color */
	int hue;		/* -180 .. 180, default 0 */
	int saturation; /* 0 .. 127, default 64 */
};

struct nano_video_device {
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct vb2_queue queue;
	struct mutex lock;
	struct nx_videolayer vl;
};

static unsigned round_up8(unsigned val)
{
	return (val + 7) / 8 * 8;
}

enum PlaneUpdateKind {
	PUK_INPUT_FORMAT,
	PUK_VB2_BUF,
	PUK_OVERLAY_WINDOW
};

static int dp_plane_update(struct nx_videolayer *vl, enum PlaneUpdateKind puk,
		struct vb2_buffer *switchBuf)
{
	dma_addr_t uvdma[2];
	bool enable, isDirtyPre, isDirtyPost, haveWork;
	unsigned pitchesY;
	phys_addr_t physaddr;
	struct vb2_buffer *bufPrev, *doneBufs[2] = { NULL, NULL };
	unsigned long flags;

	if( puk != PUK_VB2_BUF )
		switchBuf = vl->bufWantDisplay;
	enable = vl->isVidiocOverlayOn && vl->format &&
		switchBuf && vl->width && vl->height &&
		vl->dst_width && vl->dst_height;
	isDirtyPre = nx_soc_dp_plane_video_is_dirty(vl->module);
	if( enable ) {
		const struct NanoVideoFormat *nvf = vl->format;

		if( !vl->isEnabled || puk == PUK_OVERLAY_WINDOW ) {
			nx_soc_dp_plane_video_set_position(vl->module,
					0, 0, vl->width, vl->height,
					vl->dst_left, vl->dst_top,
					vl->dst_width, vl->dst_height);
		}
		if( !vl->isEnabled || puk == PUK_INPUT_FORMAT )
			nx_soc_dp_plane_video_set_format(vl->module, nvf->nxfmt);
		if( !vl->isEnabled || puk == PUK_VB2_BUF ) {
			pitchesY = round_up8(vl->width * nvf->bpp);
			physaddr = vb2_dma_contig_plane_dma_addr(switchBuf, 0);
			if( nvf->hsub ) {	// planar format
				unsigned pitchesUV = pitchesY / nvf->hsub;
				if( !vl->useSingleBuf && switchBuf->num_planes == 3 ) {
					uvdma[nvf->isVU] =
						vb2_dma_contig_plane_dma_addr(switchBuf, 1);
					uvdma[!nvf->isVU] =
						vb2_dma_contig_plane_dma_addr(switchBuf, 2);
				}else{
					uvdma[nvf->isVU] = physaddr + pitchesY * vl->height;
					uvdma[!nvf->isVU] = uvdma[nvf->isVU] +
						pitchesUV * vl->height / nvf->vsub;
				}
				nx_soc_dp_plane_video_set_address_3p( vl->module, 0, 0,
						nvf->nxfmt, physaddr, pitchesY,
						uvdma[0], pitchesUV, uvdma[1], pitchesUV);
			}else{
				nx_soc_dp_plane_video_set_address_1p(vl->module, 0, 0,
						physaddr, pitchesY);
			}
		}
	}
	if( enable != vl->isEnabled )
		nx_soc_dp_plane_video_set_enable(vl->module, enable);

	spin_lock_irqsave(&vl->wq_lock, flags);
	isDirtyPost = nx_soc_dp_plane_video_is_dirty(vl->module);
	bufPrev = vl->isEnabled ? vl->bufWantDisplay : NULL;
	if( isDirtyPost ) {
		if( bufPrev != switchBuf &&
				bufPrev != vl->bufsActiveTillDirty[0] &&
				bufPrev != vl->bufsActiveTillDirty[1])
			doneBufs[0] = bufPrev;
	}else{
		if( vl->bufsActiveTillDirty[0] != bufPrev &&
				vl->bufsActiveTillDirty[0] != switchBuf )
			doneBufs[0] = vl->bufsActiveTillDirty[0];
		if( vl->bufsActiveTillDirty[1] != bufPrev &&
				vl->bufsActiveTillDirty[1] != switchBuf )
			doneBufs[1] = vl->bufsActiveTillDirty[1];
		vl->bufsActiveTillDirty[0] = bufPrev;
		if( isDirtyPre && bufPrev != switchBuf ) {
			// in doubt which one is active now
			vl->bufsActiveTillDirty[1] = switchBuf;
		}else
			vl->bufsActiveTillDirty[1] = NULL;
	}
	vl->bufWantDisplay = switchBuf;
	vl->isEnabled = enable;
	if( !vl->isWorkActive ) {
		haveWork = ((vl->bufsActiveTillDirty[0] &&
				vl->bufsActiveTillDirty[0] != vl->bufWantDisplay) ||
			(vl->bufsActiveTillDirty[1] &&
			 vl->bufsActiveTillDirty[1] != vl->bufWantDisplay));
		vl->isWorkActive = haveWork;
	}else
		haveWork = false;
	nx_soc_dp_plane_video_set_dirty(vl->module);
	spin_unlock_irqrestore(&vl->wq_lock, flags);

	if( doneBufs[0] )
		vb2_buffer_done(doneBufs[0], VB2_BUF_STATE_DONE);
	if( doneBufs[1] )
		vb2_buffer_done(doneBufs[1], VB2_BUF_STATE_DONE);
	if( haveWork )
		queue_work(vl->workqueue, &vl->buf_switch_wait_work);
	return 0;
}

static void buf_switch_wait_work(struct work_struct *work)
{
	struct nx_videolayer *vl;
	bool isDirty, haveWork;
	struct vb2_buffer *doneBufs[2] = { NULL, NULL };
	unsigned long flags;
	unsigned timeout = 50;

	vl = container_of(work, struct nx_videolayer, buf_switch_wait_work);

	/* Waiting for dirty flag cleanup to give back unused buffers. */
	while( 1 ) {
		spin_lock_irqsave(&vl->wq_lock, flags);

		// timed wait as of dirty flag may be not cleaned when display is off
		isDirty = --timeout && nx_soc_dp_plane_video_is_dirty(vl->module);
		if( isDirty ) {
			haveWork = (vl->bufsActiveTillDirty[0] &&
					vl->bufsActiveTillDirty[0] != vl->bufWantDisplay) ||
				(vl->bufsActiveTillDirty[1] &&
				 vl->bufsActiveTillDirty[1] != vl->bufWantDisplay);
		}else{
			if( vl->bufsActiveTillDirty[0] != vl->bufWantDisplay )
				doneBufs[0] = vl->bufsActiveTillDirty[0];
			if( vl->bufsActiveTillDirty[1] != vl->bufWantDisplay )
				doneBufs[1] = vl->bufsActiveTillDirty[1];
			vl->bufsActiveTillDirty[0] = vl->isEnabled ?
				vl->bufWantDisplay : NULL;
			vl->bufsActiveTillDirty[1] = NULL;
			haveWork = false;
		}
		vl->isWorkActive = haveWork;
		spin_unlock_irqrestore(&vl->wq_lock, flags);

		if( ! haveWork )
			break;
		msleep(2);
	}
	if( doneBufs[0] )
		vb2_buffer_done(doneBufs[0], VB2_BUF_STATE_DONE);
	if( doneBufs[1] )
		vb2_buffer_done(doneBufs[1], VB2_BUF_STATE_DONE);
}

static int nano_video_vidioc_querycap(struct file *file, void *fh,
			       struct v4l2_capability *cap)
{
	/* Reporting both single- and multi-plane support in multiplane mode
	 * due to ugly libv4l-mplane plugin in libv4l library used by v4l2sink
	 * gstreamer plugin. The libv4l-mplane plugin, when enabled, makes the
	 * device visible as single-plane only and causes the v4l2sink fail to
	 * play. When both single- and multi-plane support is reported by device,
	 * the plugin is not turned on and v4l2sink plays video correctly.
	 */
	unsigned caps = V4L2_CAP_VIDEO_OUTPUT_OVERLAY | V4L2_CAP_READWRITE |
		V4L2_CAP_STREAMING | V4L2_CAP_VIDEO_OUTPUT |
		(single_plane_mode ? 0 : V4L2_CAP_VIDEO_OUTPUT_MPLANE);

	strlcpy(cap->driver, "nano-videodev", sizeof(cap->driver));
	strlcpy(cap->card, "Nexell gpu overlay video", sizeof(cap->card));
	strlcpy(cap->bus_info, "platform:mlc0", sizeof(cap->bus_info));
	cap->capabilities |= caps;
	cap->device_caps |= caps;
	return 0;
}

static int nano_video_vidioc_enum_fmt_vid_out(struct file *file,
		void *fh, struct v4l2_fmtdesc *f)
{
	if( f->index >= ARRAY_SIZE(supported_video_formats) )
		return -EINVAL;
	if( single_plane_mode && supported_video_formats[f->index].num_planes > 1)
		return -EINVAL;
	f->pixelformat = supported_video_formats[f->index].fourcc;
	return 0;
}

static int nano_video_vidioc_enum_framesizes(struct file *file, void *fh,
				      struct v4l2_frmsizeenum *fsize)
{
	if( fsize->index != 0 )
		return -EINVAL;
	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width = 2;
	fsize->stepwise.max_width = 2048;
	fsize->stepwise.step_width = 2;
	fsize->stepwise.min_height = 2;
	fsize->stepwise.max_height = 2048;
	fsize->stepwise.step_height = 2;
	return 0;
}

#if 0
static int nano_video_vidioc_enum_frameintervals(struct file *file, void *fh,
					  struct v4l2_frmivalenum *fival)
{
	if( fival->index != 0 )
		return -EINVAL;
	fival->type = V4L2_FRMIVAL_TYPE_CONTINUOUS;
	fival->stepwise.min.numerator = 0;
	fival->stepwise.min.denominator = 1;
	fival->stepwise.step.numerator = 1;
	fival->stepwise.step.denominator = 1;
	fival->stepwise.max.numerator = INT_MAX;
	fival->stepwise.max.denominator = 1;
	return 0;
}
#endif

static int nano_video_vidioc_g_fmt_vid_out(struct file *file, void *fh,
				    struct v4l2_format *f)
{
	const struct nx_videolayer *vl = video_drvdata(file);
	const struct NanoVideoFormat *nvf = vl->format;
	unsigned stride;

	f->fmt.pix.width = vl->width;
	f->fmt.pix.height = vl->height;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.pixelformat = nvf->fourcc;
	f->fmt.pix.bytesperline = stride = round_up8(vl->width * nvf->bpp);
	f->fmt.pix.sizeimage = stride * vl->height;
	if( nvf->hsub )
		f->fmt.pix.sizeimage += 2 * stride / nvf->hsub * vl->height / nvf->vsub;
	return 0;
}

static int nano_video_vidioc_s_fmt_vid_out(struct file *file, void *fh,
				    struct v4l2_format *f)
{
	struct nx_videolayer *vl = video_drvdata(file);
	const struct NanoVideoFormat *nvf;
	unsigned i, stride;

	for(i = 0; i < ARRAY_SIZE(supported_video_formats) &&
		supported_video_formats[i].fourcc != f->fmt.pix.pixelformat; ++i)
		;
	if( i == ARRAY_SIZE(supported_video_formats) ) {
		pr_err("nano-videodev: unsupported fourcc %.4s\n",
			   (const char*)&f->fmt.pix.pixelformat);
		return -EINVAL;
	}
	nvf = supported_video_formats + i;
	if( single_plane_mode && nvf->num_planes > 1 ) {
		pr_err("nano-videodev: format %.4s is multi-plane, invalid\n",
			   (const char*)&f->fmt.pix.pixelformat);
		return -EINVAL;
	}
	vl->format = nvf;
	f->fmt.pix.width += f->fmt.pix.width & 1;
	f->fmt.pix.height += f->fmt.pix.height & 1;
	vl->width = f->fmt.pix.width;
	vl->height = f->fmt.pix.height;
	vl->useSingleBuf = true;
	dp_plane_update(vl, PUK_INPUT_FORMAT, NULL);
	f->fmt.pix.bytesperline = stride = round_up8(vl->width * nvf->bpp);
	f->fmt.pix.sizeimage = stride * vl->height;
	if( nvf->hsub )
		f->fmt.pix.sizeimage += 2 * stride / nvf->hsub * vl->height / nvf->vsub;
	return 0;
}

static int nano_video_vidioc_try_fmt_vid_out(struct file *file, void *fh,
				    struct v4l2_format *f)
{
	const struct NanoVideoFormat *nvf;
	unsigned i, stride;

	for(i = 0; i < ARRAY_SIZE(supported_video_formats) &&
		supported_video_formats[i].fourcc != f->fmt.pix.pixelformat; ++i)
		;
	if( i == ARRAY_SIZE(supported_video_formats) ) {
		pr_err("nano-videodev: unsupported fourcc %.4s\n",
			   (const char*)&f->fmt.pix.pixelformat);
		return -EINVAL;
	}
	nvf = supported_video_formats + i;
	if( single_plane_mode && nvf->num_planes > 1 ) {
		pr_err("nano-videodev: format %.4s is multi-plane, invalid\n",
			   (const char*)&f->fmt.pix.pixelformat);
		return -EINVAL;
	}
	f->fmt.pix.width += f->fmt.pix.width & 1;
	f->fmt.pix.height += f->fmt.pix.height & 1;
	f->fmt.pix.bytesperline = stride = round_up8(f->fmt.pix.width * nvf->bpp);
	f->fmt.pix.sizeimage = stride * f->fmt.pix.height;
	if( nvf->hsub )
		f->fmt.pix.sizeimage += 2 * stride / nvf->hsub * f->fmt.pix.height /
			nvf->vsub;
	return 0;
}

static void fill_bytesperline_and_sizeimage_mp(struct v4l2_format *f,
		const struct NanoVideoFormat *nvf)
{
	unsigned stride;

	stride = round_up8(f->fmt.pix_mp.width * nvf->bpp);
	f->fmt.pix_mp.plane_fmt[0].bytesperline = stride;
	f->fmt.pix_mp.plane_fmt[0].sizeimage = stride * f->fmt.pix_mp.height;
	if( nvf->hsub ) {	// planar format
		unsigned strideUV = stride / nvf->hsub;
		unsigned sizeUV = strideUV * f->fmt.pix_mp.height / nvf->vsub;
		if( f->fmt.pix_mp.num_planes == 3 ) {
			f->fmt.pix_mp.plane_fmt[1].bytesperline =
				f->fmt.pix_mp.plane_fmt[2].bytesperline = strideUV;
			f->fmt.pix_mp.plane_fmt[1].sizeimage =
				f->fmt.pix_mp.plane_fmt[2].sizeimage = sizeUV;
		}else	// planes in one buffer
			f->fmt.pix_mp.plane_fmt[0].sizeimage += 2 * sizeUV;
	}
}

static int nano_video_vidioc_g_fmt_vid_out_mplane(struct file *file, void *fh,
					   struct v4l2_format *f)
{
	struct nx_videolayer *vl = video_drvdata(file);
	const struct NanoVideoFormat *nvf = vl->format;

	f->fmt.pix_mp.width = vl->width;
	f->fmt.pix_mp.height = vl->height;
	f->fmt.pix_mp.field = V4L2_FIELD_NONE;
	f->fmt.pix_mp.pixelformat = nvf->fourcc;
	f->fmt.pix_mp.num_planes = vl->useSingleBuf ? 1 : nvf->num_planes;
	fill_bytesperline_and_sizeimage_mp(f, nvf);
	return 0;
}

static int nano_video_vidioc_try_fmt_vid_out_mplane(struct file *file, void *fh,
					   struct v4l2_format *f)
{
	const struct NanoVideoFormat *nvf = NULL;
	unsigned i;

	for(i = 0; i < ARRAY_SIZE(supported_video_formats) &&
			supported_video_formats[i].fourcc != f->fmt.pix_mp.pixelformat; ++i)
		;
	if( i == ARRAY_SIZE(supported_video_formats) ) {
		pr_err("nano-videodev: unsupported fourcc %.4s\n",
				(const char*)&f->fmt.pix_mp.pixelformat);
		return -EINVAL;
	}
	nvf = supported_video_formats + i;
	f->fmt.pix_mp.width += f->fmt.pix_mp.width & 1;
	f->fmt.pix_mp.height += f->fmt.pix_mp.height & 1;
	f->fmt.pix_mp.field = V4L2_FIELD_NONE;
	if( f->fmt.pix_mp.num_planes != 1 )
		f->fmt.pix_mp.num_planes = nvf->num_planes;
	fill_bytesperline_and_sizeimage_mp(f, nvf);
	return 0;
}

static int nano_video_vidioc_s_fmt_vid_out_mplane(struct file *file, void *fh,
					   struct v4l2_format *f)
{
	struct nx_videolayer *vl = video_drvdata(file);
	const struct NanoVideoFormat *nvf = NULL;
	unsigned i;

	for(i = 0; i < ARRAY_SIZE(supported_video_formats) &&
			supported_video_formats[i].fourcc != f->fmt.pix_mp.pixelformat; ++i)
		;
	if( i == ARRAY_SIZE(supported_video_formats) ) {
		pr_err("nano-videodev: unsupported fourcc %.4s\n",
				(const char*)&f->fmt.pix_mp.pixelformat);
		return -EINVAL;
	}
	nvf = supported_video_formats + i;
	vl->format = nvf;
	f->fmt.pix_mp.width += f->fmt.pix_mp.width & 1;
	f->fmt.pix_mp.height += f->fmt.pix_mp.height & 1;
	vl->width = f->fmt.pix_mp.width;
	vl->height = f->fmt.pix_mp.height;
	vl->useSingleBuf = f->fmt.pix_mp.num_planes == 1;
	dp_plane_update(vl, PUK_INPUT_FORMAT, NULL);
	f->fmt.pix_mp.field = V4L2_FIELD_NONE;
	if( ! vl->useSingleBuf )
		f->fmt.pix_mp.num_planes = nvf->num_planes;
	fill_bytesperline_and_sizeimage_mp(f, nvf);
	return 0;
}

static int nano_video_vidioc_g_fmt_vid_out_overlay(struct file *file, void *fh,
					    struct v4l2_format *f)
{
	struct nx_videolayer *vl = video_drvdata(file);
	f->fmt.win.w.left = vl->dst_left;
	f->fmt.win.w.top = vl->dst_top;
	f->fmt.win.w.width = vl->dst_width;
	f->fmt.win.w.height = vl->dst_height;
	f->fmt.win.field = V4L2_FIELD_NONE;
	return 0;
}

static int nano_video_vidioc_s_fmt_vid_out_overlay(struct file *file, void *fh,
					    struct v4l2_format *f)
{
	struct nx_videolayer *vl = video_drvdata(file);

	vl->dst_left = f->fmt.win.w.left;
	vl->dst_top = f->fmt.win.w.top;
	vl->dst_width = f->fmt.win.w.width;
	vl->dst_height = f->fmt.win.w.height;
	dp_plane_update(vl, PUK_OVERLAY_WINDOW, NULL);
	return 0;
}

static int nano_video_vidioc_overlay(struct file *file, void *fh, unsigned i)
{
	struct nx_videolayer *vl = video_drvdata(file);

	vl->isVidiocOverlayOn = i != 0;
	dp_plane_update(vl, PUK_OVERLAY_WINDOW, NULL);
	return 0;
}

static int nano_video_vidioc_queryctrl(struct file *file, void *fh,
				struct v4l2_queryctrl *a)
{
	static const unsigned supportedIds[] = {
		V4L2_CID_BRIGHTNESS,
		V4L2_CID_CONTRAST,
		V4L2_CID_SATURATION,
		V4L2_CID_HUE,
		V4L2_CID_ALPHA_COMPONENT,
		V4L2_CID_PRIVATE_BASE
	};
	unsigned i, controlId, nextId = 0;

	controlId = a->id &
		~(V4L2_CTRL_FLAG_NEXT_CTRL|V4L2_CTRL_FLAG_NEXT_COMPOUND);
	if( a->id & V4L2_CTRL_FLAG_NEXT_CTRL ) {
		for(i = 0; i < ARRAY_SIZE(supportedIds); ++i) {
			if( supportedIds[i] > controlId && (nextId == 0 ||
						nextId > supportedIds[i]) )
				nextId = supportedIds[i];
		}
		if( nextId == 0 ) 
			return -EINVAL;
		controlId = nextId;
	}
	a->id = controlId;
	switch( controlId ) {
	case V4L2_CID_BRIGHTNESS:
		a->type = V4L2_CTRL_TYPE_INTEGER;
		strlcpy(a->name, "brightness", sizeof(a->name));
		a->minimum = -128;
		a->maximum = 127;
		a->step = 1;
		a->default_value = 0;
		a->flags = 0;
		break;
	case V4L2_CID_CONTRAST:
		a->type = V4L2_CTRL_TYPE_INTEGER;
		strlcpy(a->name, "contrast", sizeof(a->name));
		a->minimum = 0;
		a->maximum = 7;
		a->step = 1;
		a->default_value = 0;
		a->flags = 0;
		break;
	case V4L2_CID_SATURATION:
		a->type = V4L2_CTRL_TYPE_INTEGER;
		strlcpy(a->name, "saturation", sizeof(a->name));
		a->minimum = 0;
		a->maximum = 127;
		a->step = 1;
		a->default_value = 64;
		a->flags = 0;
		break;
	case V4L2_CID_HUE:
		a->type = V4L2_CTRL_TYPE_INTEGER;
		strlcpy(a->name, "saturation", sizeof(a->name));
		a->minimum = -180;
		a->maximum = 179;
		a->step = 1;
		a->default_value = 0;
		a->flags = 0;
		break;
	case V4L2_CID_ALPHA_COMPONENT:
		a->type = V4L2_CTRL_TYPE_INTEGER;
		strlcpy(a->name, "alpha", sizeof(a->name));
		a->minimum = 0;
		a->maximum = 255;
		a->step = 1;
		a->default_value = 255;
		a->flags = 0;
		break;
	case V4L2_CID_PRIVATE_BASE:
		a->type = V4L2_CTRL_TYPE_INTEGER;
		strlcpy(a->name, "layer z-order", sizeof(a->name));
		a->minimum = 0;
		a->maximum = 3;
		a->step = 1;
		a->default_value = 2;
		a->flags = 0;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void setHueSaturationOnMlc(const struct nx_videolayer *vl)
{
	/* sin(0) .. sin(90) multiplied by 65535 */
	static const unsigned short sine_table[91] = {
		    0,  1143,  2287,  3429,  4571,  5711,  6850,  7986,  9120, 10251,
		11380, 12504, 13625, 14742, 15854, 16961, 18063, 19160, 20251, 21336,
		22414, 23485, 24549, 25606, 26655, 27696, 28728, 29752, 30766, 31771,
		32767, 33753, 34728, 35692, 36646, 37589, 38520, 39439, 40347, 41242,
		42125, 42994, 43851, 44694, 45524, 46340, 47141, 47929, 48701, 49459,
		50202, 50930, 51642, 52338, 53018, 53683, 54330, 54962, 55576, 56174,
		56754, 57318, 57863, 58392, 58902, 59394, 59869, 60325, 60762, 61182,
		61582, 61964, 62327, 62671, 62996, 63301, 63588, 63855, 64102, 64330,
		64539, 64728, 64897, 65046, 65175, 65285, 65375, 65445, 65495, 65525,
		65535 
	};
	int sine, cosine;

	if( vl->hue < -90 ) {
		sine = -sine_table[ vl->hue + 180 ];
		cosine = -sine_table[ -90 - vl->hue ];
	}else if( vl->hue < 0 ) {
		sine = -sine_table[ -vl->hue ];
		cosine = sine_table[ 90 + vl->hue ];
	}else if( vl->hue < 90 ) {
		sine = sine_table[ vl->hue ];
		cosine = sine_table[ 90 - vl->hue ];
	}else{
		sine = sine_table[ 180 - vl->hue ];
		cosine = -sine_table[ vl->hue - 90 ];
	}
	sine = (sine * vl->saturation) / 65535;
	cosine = (cosine * vl->saturation) / 65535;
	nx_mlc_set_video_layer_chroma_enhance(vl->module, 0,
			cosine, -sine, sine, cosine);
}

int nano_video_vidioc_g_ctrl(struct file *file, void *fh,
			     struct v4l2_control *a)
{
	struct nx_videolayer *vl = video_drvdata(file);

	switch( a->id ) {
	case V4L2_CID_BRIGHTNESS:
		a->value = nx_mlc_get_video_layer_brightness(vl->module);
		break;
	case V4L2_CID_CONTRAST:
		a->value = nx_mlc_get_video_layer_contrast(vl->module);
		break;
	case V4L2_CID_SATURATION:
		a->value = vl->saturation;
		break;
	case V4L2_CID_HUE:
		a->value = vl->hue;
		break;
	case V4L2_CID_ALPHA_COMPONENT:
		a->value = nx_mlc_get_layer_alpha256(vl->module, 3);
		break;
	case V4L2_CID_PRIVATE_BASE:
		a->value = nx_soc_dp_plane_video_get_priority(vl->module);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

int nano_video_vidioc_s_ctrl(struct file *file, void *fh,
			     struct v4l2_control *a)
{
	struct nx_videolayer *vl = video_drvdata(file);

	switch( a->id ) {
	case V4L2_CID_BRIGHTNESS:
		nx_mlc_set_video_layer_brightness(vl->module,
				clamp_val(a->value, -128, 127));
		break;
	case V4L2_CID_CONTRAST:
		nx_mlc_set_video_layer_contrast(vl->module,
				clamp_val(a->value, 0, 7));
		break;
	case V4L2_CID_SATURATION:
		vl->saturation = clamp_val(a->value, 0, 127);
		setHueSaturationOnMlc(vl);
		break;
	case V4L2_CID_HUE:
		vl->hue = clamp_val(a->value, -180, 180);
		setHueSaturationOnMlc(vl);
		break;
	case V4L2_CID_ALPHA_COMPONENT:
		nx_mlc_set_layer_alpha256(vl->module, 3, clamp_val(a->value, 0, 255));
		break;
	case V4L2_CID_PRIVATE_BASE:
		nx_soc_dp_plane_video_set_priority(vl->module,
				clamp_val(a->value, 0, 3));
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct v4l2_ioctl_ops nano_video_ioctl_ops = {
	.vidioc_querycap				= nano_video_vidioc_querycap,
	.vidioc_enum_fmt_vid_out		= nano_video_vidioc_enum_fmt_vid_out,
	.vidioc_enum_framesizes			= nano_video_vidioc_enum_framesizes,
	//.vidioc_enum_frameintervals		= nano_video_vidioc_enum_frameintervals,
	.vidioc_g_fmt_vid_out			= nano_video_vidioc_g_fmt_vid_out,
	.vidioc_try_fmt_vid_out			= nano_video_vidioc_try_fmt_vid_out,
	.vidioc_s_fmt_vid_out			= nano_video_vidioc_s_fmt_vid_out,
	.vidioc_g_fmt_vid_out_overlay	= nano_video_vidioc_g_fmt_vid_out_overlay,
	.vidioc_s_fmt_vid_out_overlay	= nano_video_vidioc_s_fmt_vid_out_overlay,
	.vidioc_overlay					= nano_video_vidioc_overlay,
	.vidioc_queryctrl				= nano_video_vidioc_queryctrl,
	.vidioc_g_ctrl					= nano_video_vidioc_g_ctrl,
	.vidioc_s_ctrl					= nano_video_vidioc_s_ctrl,
	.vidioc_reqbufs					= vb2_ioctl_reqbufs,
	.vidioc_create_bufs       		= vb2_ioctl_create_bufs,
	.vidioc_prepare_buf       		= vb2_ioctl_prepare_buf,
	.vidioc_querybuf          		= vb2_ioctl_querybuf,
	.vidioc_qbuf              		= vb2_ioctl_qbuf,
	.vidioc_dqbuf             		= vb2_ioctl_dqbuf,
	.vidioc_expbuf            		= vb2_ioctl_expbuf,
	.vidioc_streamon          		= vb2_ioctl_streamon,
	.vidioc_streamoff         		= vb2_ioctl_streamoff,

};

static const struct v4l2_ioctl_ops nano_video_ioctl_ops_mp = {
	.vidioc_querycap				= nano_video_vidioc_querycap,
	.vidioc_enum_fmt_vid_out		= nano_video_vidioc_enum_fmt_vid_out,
	.vidioc_enum_fmt_vid_out_mplane = nano_video_vidioc_enum_fmt_vid_out,
	.vidioc_enum_framesizes			= nano_video_vidioc_enum_framesizes,
	//.vidioc_enum_frameintervals		= nano_video_vidioc_enum_frameintervals,
	.vidioc_g_fmt_vid_out			= nano_video_vidioc_g_fmt_vid_out,
	.vidioc_g_fmt_vid_out_mplane	= nano_video_vidioc_g_fmt_vid_out_mplane,
	.vidioc_try_fmt_vid_out			= nano_video_vidioc_try_fmt_vid_out,
	.vidioc_try_fmt_vid_out_mplane	= nano_video_vidioc_try_fmt_vid_out_mplane,
	.vidioc_s_fmt_vid_out			= nano_video_vidioc_s_fmt_vid_out,
	.vidioc_s_fmt_vid_out_mplane	= nano_video_vidioc_s_fmt_vid_out_mplane,
	.vidioc_g_fmt_vid_out_overlay	= nano_video_vidioc_g_fmt_vid_out_overlay,
	.vidioc_s_fmt_vid_out_overlay	= nano_video_vidioc_s_fmt_vid_out_overlay,
	.vidioc_queryctrl				= nano_video_vidioc_queryctrl,
	.vidioc_g_ctrl					= nano_video_vidioc_g_ctrl,
	.vidioc_s_ctrl					= nano_video_vidioc_s_ctrl,
	.vidioc_overlay					= nano_video_vidioc_overlay,
	.vidioc_reqbufs					= vb2_ioctl_reqbufs,
	.vidioc_create_bufs       		= vb2_ioctl_create_bufs,
	.vidioc_prepare_buf       		= vb2_ioctl_prepare_buf,
	.vidioc_querybuf          		= vb2_ioctl_querybuf,
	.vidioc_qbuf              		= vb2_ioctl_qbuf,
	.vidioc_dqbuf             		= vb2_ioctl_dqbuf,
	.vidioc_expbuf            		= vb2_ioctl_expbuf,
	.vidioc_streamon          		= vb2_ioctl_streamon,
	.vidioc_streamoff         		= vb2_ioctl_streamoff,

};

static const struct v4l2_file_operations nano_video_fops = {
	.owner			= THIS_MODULE,
	.open			= v4l2_fh_open,
	.read			= vb2_fop_read,
	.write			= vb2_fop_write,
	.poll			= vb2_fop_poll,
	.mmap			= vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
	.release		= vb2_fop_release,
};

static int nano_video_vb2_queue_setup(struct vb2_queue *q,
		   unsigned int *num_buffers, unsigned int *num_planes,
		   unsigned int sizes[], struct device *alloc_devs[])
{
	struct nx_videolayer *vl = vb2_get_drv_priv(q);
	const struct NanoVideoFormat *nvf = vl->format;
	unsigned stride, sizeLume, sizeUV;

	stride = round_up8(vl->width * nvf->bpp);
	sizeLume = stride * vl->height;
	sizeUV = nvf->hsub ? stride / nvf->hsub * vl->height / nvf->vsub : 0;
	if( *num_planes == 0 ) {
		if( *num_buffers < 3 )
			*num_buffers = 3;
		*num_planes = vl->useSingleBuf ? 1 : nvf->num_planes;
		if( *num_planes == 3 ) {
			sizes[0] = sizeLume;
			sizes[1] = sizes[2] = sizeUV;
		}else
			sizes[0] = sizeLume + 2 * sizeUV;
	}else{	// checking additional buffers for VIDIOC_CREATE_BUFS
		if( *num_planes == vl->useSingleBuf ? 1 : nvf->num_planes ) {
			if( *num_planes == 3 ?
				sizes[0] < sizeLume || sizes[1] < sizeUV || sizes[2] < sizeUV :
				sizes[0] < sizeLume + 2 * sizeUV )
			{
				pr_err("nano-videodev: buffer size too small\n");
				return -EINVAL;
			}
		}else{
			pr_err("nano-videodev: num planes mismatch\n");
			return -EINVAL;
		}
	}
	return 0;
}

static void nano_video_vb2_buf_queue(struct vb2_buffer *vb)
{
	struct nx_videolayer *vl = vb2_get_drv_priv(vb->vb2_queue);

	dp_plane_update(vl, PUK_VB2_BUF, vb);
}

static int nano_video_vb2_start_streaming(struct vb2_queue *q, unsigned count)
{
	struct nx_videolayer *vl = vb2_get_drv_priv(q);

	vl->isVidiocOverlayOn = true;
	dp_plane_update(vl, PUK_INPUT_FORMAT, NULL);
	return 0;
}

static void nano_video_vb2_stop_streaming(struct vb2_queue *q)
{
	struct nx_videolayer *vl = vb2_get_drv_priv(q);

	vl->isVidiocOverlayOn = false;
	dp_plane_update(vl, PUK_VB2_BUF, NULL);
	flush_work(&vl->buf_switch_wait_work);
}

static struct vb2_ops nano_video_vb2_queue_ops = {
	.queue_setup			= nano_video_vb2_queue_setup,
	.buf_queue				= nano_video_vb2_buf_queue,
	.start_streaming		= nano_video_vb2_start_streaming,
	.stop_streaming			= nano_video_vb2_stop_streaming,
	.wait_prepare           = vb2_ops_wait_prepare,
	.wait_finish            = vb2_ops_wait_finish,
};

static void nano_video_device_release(struct video_device *vdev)
{
	/* print out information to verify that module was unloaded correctly */
	pr_info("nano-videodev: released video device\n");
}

static int nano_video_probe(struct platform_device *pdev)
{
	int ret;
	struct resource *res;
	void *reg_base;
	struct nano_video_device *nvdev;

	nvdev = devm_kzalloc(&pdev->dev, sizeof(struct nano_video_device),
			GFP_KERNEL);
	if( nvdev == NULL )
		return -ENOMEM;
	strlcpy(nvdev->vdev.name, "nano-videodev", sizeof(nvdev->vdev.name));
	nvdev->vdev.vfl_dir = VFL_DIR_TX;
	nvdev->vdev.fops = &nano_video_fops;
	nvdev->vdev.release = nano_video_device_release;
	nvdev->vdev.ioctl_ops = single_plane_mode ?  &nano_video_ioctl_ops :
		&nano_video_ioctl_ops_mp;
	nvdev->vl.module = 0;
	nvdev->vl.format = NULL;
	nvdev->vl.hue = 0;
	nvdev->vl.saturation = 64;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if(res == NULL) {
		dev_err(&pdev->dev, "failed to get registers base address\n");
		return -ENXIO;
	}
	reg_base = devm_ioremap_resource(&pdev->dev, res);
	if( reg_base == NULL ) {
		dev_err(&pdev->dev, "failed to iomap MLC\n");
		return -EIO;
	}
	nx_mlc_set_base_address(nvdev->vl.module, reg_base);
	ret = v4l2_device_register(&pdev->dev, &nvdev->v4l2_dev);
	if( ret ) {
		dev_err(&pdev->dev, "failed to register v4l2 device\n");
		return ret;
	}
	mutex_init(&nvdev->lock);
	spin_lock_init(&nvdev->vl.wq_lock);
	nvdev->vl.workqueue = alloc_workqueue("nano-videodev",
			WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	INIT_WORK(&nvdev->vl.buf_switch_wait_work, buf_switch_wait_work);
	if( ! nvdev->vl.workqueue ) {
		dev_err(&pdev->dev, "unable to alloc workqueue\n");
		ret = -ENOMEM;
		goto err_unregister_v4l2;
	}

	nvdev->queue.type = single_plane_mode ? V4L2_BUF_TYPE_VIDEO_OUTPUT :
		V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	nvdev->queue.io_modes = VB2_MMAP | VB2_USERPTR | VB2_WRITE |
		VB2_DMABUF;
	nvdev->queue.dev = &pdev->dev;
	nvdev->queue.lock = &nvdev->lock;
	nvdev->queue.ops = &nano_video_vb2_queue_ops;
	nvdev->queue.mem_ops = &vb2_dma_contig_memops;
	nvdev->queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	nvdev->queue.drv_priv = &nvdev->vl;
	ret = vb2_queue_init(&nvdev->queue);
	if( ret ) {
		dev_err(&pdev->dev, "queue init failed\n");
		goto err_destroy_workqueue;
	}
	nvdev->vdev.queue = &nvdev->queue;
	nvdev->vdev.v4l2_dev = &nvdev->v4l2_dev;
	video_set_drvdata(&nvdev->vdev, &nvdev->vl);
	ret = video_register_device(&nvdev->vdev, VFL_TYPE_GRABBER, 1);
	if( ret ) {
		dev_err(&pdev->dev, "failed to register video device\n");
		goto err_queue_release;
	}
	dev_info(&pdev->dev, "registered video device %s\n", nvdev->vdev.name);
	nvdev->vl.format = supported_video_formats; // first format is default
	nvdev->vl.width = 640;
	nvdev->vl.height = 360;
	nx_mlc_get_screen_size(0, &nvdev->vl.dst_width,
				   &nvdev->vl.dst_height);
	if( nvdev->vl.dst_width > 1920 ) {
		nvdev->vl.dst_left = (nvdev->vl.dst_width - 1920) / 2;
		nvdev->vl.dst_width = 1920;
	}
	if( nvdev->vl.dst_height > 1080 ) {
		nvdev->vl.dst_top = (nvdev->vl.dst_height - 1080) / 2;
		nvdev->vl.dst_height = 1080;
	}
	platform_set_drvdata(pdev, nvdev);
	setHueSaturationOnMlc(&nvdev->vl);
	return 0;
err_queue_release:
	vb2_queue_release(&nvdev->queue);
err_destroy_workqueue:
	destroy_workqueue(nvdev->vl.workqueue);
err_unregister_v4l2:
	v4l2_device_unregister(&nvdev->v4l2_dev);
	return ret;
}

static int nano_video_remove(struct platform_device *pdev)
{
	struct nano_video_device *nvdev = platform_get_drvdata(pdev);

	video_unregister_device(&nvdev->vdev);
	vb2_queue_release(&nvdev->queue);
	destroy_workqueue(nvdev->vl.workqueue);
	v4l2_device_unregister(&nvdev->v4l2_dev);
	pr_info("nano-videodev: unregistered v4l2 device\n");
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id nano_video_of_match[] = {
	{ .compatible = "nexell,nano-videodev" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, nano_video_of_match);
#endif

static struct platform_driver nano_video_platform_driver = {
	.probe = nano_video_probe,
	.remove = nano_video_remove,
	.driver = {
		.name = "nano-videodev",
		.of_match_table = of_match_ptr(nano_video_of_match),
	},
};

static int __init nano_video_init(void)
{
	int ret = platform_driver_register(&nano_video_platform_driver);
	pr_info("nano-videodev: registered platform driver\n");
	return ret;
}

static void __exit nano_video_exit(void)
{
	platform_driver_unregister(&nano_video_platform_driver);
	pr_info("nano-videodev: unregistered platform driver\n");
}

module_init(nano_video_init);
module_exit(nano_video_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rafaello7 <fatwildcat@gmail.com>");
MODULE_DESCRIPTION("Expose MLC video layer of s5p6818 as v4l2 device");
