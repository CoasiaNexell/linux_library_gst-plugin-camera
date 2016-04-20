/*
 * Copyright (C) 2016  Nexell Co., Ltd.
 * Author: Sungwoo, Park <swpark@nexell.co.kr>
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

#ifdef HAVA_CONFIG_H
#include "config.h"
#endif

#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdlib.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <linux/videodev2.h>

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <gst/gstutils.h>
#include <gst/video/video-info.h>
#include <glib-object.h>

#include "media-bus-format.h"

#include "nx-v4l2.h"

#include "gstcamerasrc.h"

#ifdef USE_NATIVE_DRM_BUFFER
#include "nx-drm-allocator.h"
#else
#include <tbm_bufmgr.h>
#endif

GST_DEBUG_CATEGORY(camerasrc_debug);
#define GST_CAT_DEFAULT camerasrc_debug

/* set/get property args */
enum {
	ARG_0,
	/* camera */
	ARG_CAMERA_ID, /* module number */
	ARG_CAMERA_CROP_X,
	ARG_CAMERA_CROP_Y,
	ARG_CAMERA_CROP_WIDTH,
	ARG_CAMERA_CROP_HEIGHT,

	/* capture */
	ARG_CAMERA_CAPTURE_FOURCC,
	ARG_CAMERA_CAPTURE_WIDTH,
	ARG_CAMERA_CAPTURE_HEIGHT,
	ARG_CAMERA_CAPTURE_INTERVAL,
	ARG_CAMERA_CAPTURE_COUNT,
	ARG_CAMERA_CAPTURE_JPG_QUALITY,
	ARG_CAMERA_CAPTURE_PROVIDE_EXIF,

	/* etc */
	ARG_VFLIP,
	ARG_HFLIP,

	ARG_NUM,
};

/* signals */
enum {
	SIGNAL_STILL_CAPTURE,

	SIGNAL_LAST
};

/* pre define for properties */
#define MIN_CAMERA_ID	0
#define MAX_CAMERA_ID	2

#define MAX_RESOLUTION_X	1920
#define MAX_RESOLUTION_Y	1080

#define DEF_CAPTURE_WIDTH	640
#define DEF_CAPTURE_HEIGHT	480
#define DEF_CAPTURE_COUNT	1
#define DEF_CAPTURE_INTERVAL	0
#define DEF_CAPTURE_JPG_QUALITY	95
#define DEF_CAPTURE_PROVIDE_EXIF	FALSE
#define DEF_PIXEL_FORMAT	V4L2_PIX_FMT_YUV420

#define DEF_FPS			30

#define DEF_BUFFER_COUNT	8

#define MAKE_FOURCC_FROM_STRING(string) ((guint32)(string[0]		| \
						  (string[1] << 8)	| \
						  (string[2] << 16)	| \
						  (string[3] << 24)))

/* static guint gst_camerasrc_signals[SIGNAL_LAST]; */

static GstStaticPadTemplate src_factory =
	GST_STATIC_PAD_TEMPLATE("src",
				GST_PAD_SRC,
				GST_PAD_ALWAYS,
				/* GST_STATIC_CAPS("video/x-raw," */
				/* 		"format = (string { I420 }, " */
				/* 		"width = (int) [ 1, 4096 ], " */
				/* 		"height = (int) [ 1, 4096 ]; " */
				/* 		"video/x-raw," */
				/* 		"format = (string { YV12 }, " */
				/* 		"width = (int) [ 1, 4096 ], " */
				/* 		"height = (int) [ 1, 4096 ]; " */
				/* 		"video/x-raw," */
				/* 		"format = (string { NV12 }, " */
				/* 		"width = (int) [ 1, 4096 ], " */
				/* 		"height = (int) [ 1, 4096 ]; " */
				/* 		"video/x-raw," */
				/* 		"format = (string { NV21 }, " */
				/* 		"width = (int) [ 1, 4096 ], " */
				/* 		"height = (int) [ 1, 4096 ]; ") */
				GST_STATIC_CAPS(
						 "video/x-raw"
						)
				);

/* util function */
static gboolean _get_frame_size(guint32 fourcc, int width, int height,
				guint *size)
{
	guint y_size;
	guint y_stride;

	y_stride = GST_ROUND_UP_32(width);
	y_size = y_stride * GST_ROUND_UP_16(height);

	switch (fourcc) {
	case V4L2_PIX_FMT_YUV420:
	case V4L2_PIX_FMT_YVU420:
		*size = y_size + 2 * GST_ROUND_UP_16(y_stride >> 1)
		* GST_ROUND_UP_16(height >> 1);
		break;
	case V4L2_PIX_FMT_YUYV:
	case V4L2_PIX_FMT_YVYU:
	case V4L2_PIX_FMT_UYVY:
	case V4L2_PIX_FMT_VYUY:
		*size = y_size << 1;
		break;
	case V4L2_PIX_FMT_NV21:
	case V4L2_PIX_FMT_NV12:
		*size = y_size + y_stride * GST_ROUND_UP_16(height >> 1);
		break;
	default:
		break;
	}

	return TRUE;
}

static gboolean _get_pixel_format(int fourcc, guint32 *pixel_format)
{
	switch (fourcc) {
	case GST_MAKE_FOURCC('I','4','2','0'):
		*pixel_format = V4L2_PIX_FMT_YUV420;
		break;
	case GST_MAKE_FOURCC('Y','V','1','2'):
		*pixel_format = V4L2_PIX_FMT_YVU420;
		break;
	case GST_MAKE_FOURCC('Y','U','Y','V'):
	case GST_MAKE_FOURCC('Y','U','Y','2'):
		*pixel_format = V4L2_PIX_FMT_YUYV;
		break;
	case GST_MAKE_FOURCC('Y','V','Y','U'):
		*pixel_format = V4L2_PIX_FMT_YVYU;
		break;
	case GST_MAKE_FOURCC('U','Y','V','Y'):
		*pixel_format = V4L2_PIX_FMT_UYVY;
		break;
	case GST_MAKE_FOURCC('V','Y','U','Y'):
		*pixel_format = V4L2_PIX_FMT_VYUY;
		break;
	case GST_MAKE_FOURCC('N','V','2','1'):
		*pixel_format = V4L2_PIX_FMT_NV21;
		break;
	case GST_MAKE_FOURCC('N','V','1','2'):
		*pixel_format = V4L2_PIX_FMT_NV12;
		break;
	default:
		GST_ERROR("Unknown fourcc: 0x%x", fourcc);
		return FALSE;
	}

	return TRUE;
}

static gboolean _get_caps_info(GstCameraSrc *camerasrc, GstCaps *caps,
			       guint *size)
{
	gint w;
	gint h;
	gint fps_n;
	gint fps_d;
	gchar *caps_string;
	const gchar *mime_type;
	const gchar *caps_format_name = NULL;
	guint32 pixel_format;
	guint32 caps_fourcc = 0;
	const GValue *framerate;
	GstStructure *s = NULL;
	gboolean ret;

	GST_INFO_OBJECT(camerasrc, "ENTERED Collect data for given caps.(caps:%p)", caps);

	s = gst_caps_get_structure(caps, 0);
	if (!s) {
		GST_ERROR_OBJECT(camerasrc, "failed to get structure from caps(%p)", caps);
		return FALSE;
	}

	if (!gst_structure_get_int(s, "width", &w)) {
		GST_ERROR_OBJECT(camerasrc, "failed to get width from structure(%p)", s);
		/* return FALSE; */
		w = camerasrc->width;

	}

	if (!gst_structure_get_int(s, "height", &h)) {
		GST_ERROR_OBJECT(camerasrc, "failed to get height from structure(%p)", s);
		/* return FALSE; */
		h = camerasrc->height;
	}

	if (w == 1)
		w = DEF_CAPTURE_WIDTH;

	if (h == 1)
		h = DEF_CAPTURE_HEIGHT;

	camerasrc->width = w;
	camerasrc->height = h;

	framerate = gst_structure_get_value(s, "framerate");
	if (!framerate) {
		GST_INFO("Set FPS as default(30)");

		fps_n = DEF_FPS;
		fps_d = 1;
	} else {
		fps_n = gst_value_get_fraction_numerator(framerate);
		fps_d = gst_value_get_fraction_denominator(framerate);

		if (fps_n <= 0) {
			GST_WARNING("%d: Invalid numerator of FPS, set as default(30).", fps_n);
			fps_n = DEF_FPS;
		}

		if (fps_d <= 0) {
			GST_WARNING("%d: Invalid denominator of FPS, set as default(1).", fps_d);
			fps_d = 1;
		}
	}
	camerasrc->fps = (guint)((float)fps_n / (float)fps_d);

	mime_type = gst_structure_get_name(s);
	*size = 0;
	if (!strcmp(mime_type, "video/x-raw")) {
		caps_format_name = gst_structure_get_string(s, "format");
		if (caps_format_name == NULL) {
			GST_ERROR_OBJECT(camerasrc, "failed to get caps_format_name");
			/* return FALSE; */
			goto OUT;
		}
		caps_fourcc = MAKE_FOURCC_FROM_STRING(caps_format_name);

		ret = _get_pixel_format(caps_fourcc, &pixel_format);
		if (ret == FALSE) {
			GST_ERROR_OBJECT(camerasrc,
					 "failed to get pixel format");
			return ret;
		}
		camerasrc->pixel_format = pixel_format;
	} else {
		GST_ERROR_OBJECT(camerasrc, "Unsupported mime type: %s",
				 mime_type);
		return FALSE;
	}

OUT:
	/* for debugging */
	caps_string = gst_caps_to_string(caps);
	if (caps_string) {
		GST_INFO_OBJECT(camerasrc, "caps: [%s]", caps_string);
		g_free(caps_string);
	}

	return TRUE;
}

static gboolean _create_buffer(GstCameraSrc *camerasrc)
{
#ifdef USE_NATIVE_DRM_BUFFER
	int drm_fd;
	int gem_fd;
	int dma_fd;
	void *vaddr;
	int i;

	drm_fd = open_drm_device();
	if (drm_fd < 0) {
		GST_ERROR_OBJECT(camerasrc, "failed to open drm device");
		return FALSE;
	}
	camerasrc->drm_fd = drm_fd;

	for (i = 0; i < camerasrc->buffer_count; i++) {
		gem_fd = alloc_gem(drm_fd, camerasrc->buffer_size, 0);
		if (gem_fd < 0) {
			GST_ERROR_OBJECT(camerasrc,
					 "failed to alloc gem %d", i);
			return FALSE;
		}

		dma_fd = gem_to_dmafd(drm_fd, gem_fd);
		if (dma_fd < 0) {
			GST_ERROR_OBJECT(camerasrc,
					 "failed to gem to dma %d", i);
			return FALSE;
		}

		if (get_vaddr(drm_fd, gem_fd, camerasrc->buffer_size, &vaddr)) {
			GST_ERROR_OBJECT(camerasrc,
					 "failed to get_vaddr %d", i);
			return FALSE;
		}

		camerasrc->gem_fds[i] = gem_fd;
		camerasrc->dma_fds[i] = dma_fd;
		camerasrc->vaddrs[i] = vaddr;
	}
#else
	tbm_bufmgr bufmgr;
	tbm_bo bo_alloc;
	tbm_bo_handle bo_handle_fd;
	tbm_bo_handle bo_handle_vaddr;
	int i;
	camerasrc_buffer_t *buf;
	tbm_bufmgr bufmgr;

	if (!camerasrc->bufmgr) {
		bufmgr = tbm_bufmgr_init(-1);
		if (!bufmgr) {
			GST_ERROR_OBJECT(camerasrc, "failed to tbm_bufmgr_init");
			return FALSE;
		}

		camerasrc->bufmgr = bufmgr;
	}

	for (i = 0; i < camerasrc->buffer_count; i++) {
		buf = &camerasrc->buffer[i];

		bo_alloc = tbm_bo_alloc(bufmgr, camerasrc->buffer_size,
					TBM_BO_DEFAULT);
		if (!bo_alloc) {
			GST_ERROR_OBJECT(camerasrc, "failed to tbm_bo_alloc, index %d", i);
			return FALSE;
		}

		/* get dmabuf fd */
		bo_handle_fd = tbm_bo_get_handle(bo_alloc, TBM_DEVICE_MM);
		if (!bo_handle_fd.u32) {
			tbm_bo_unref(bo_alloc);
			GST_ERROR_OBJECT(camerasrc, "failed to tbm_bo_get_handle MM, index %d", i);
			return FALSE;
		}

		/* get virtual address */
		bo_handle_vaddr = tbm_bo_get_handle(bo_alloc, TBM_DEVICE_CPU);
		if (!bo_handle_vaddr.ptr) {
			tbm_bo_unref(bo_alloc);
			GST_ERROR_OBJECT(camerasrc, "failed to tbm_bo_get_handle CPU, index %d", i);
			return FALSE;
		}

		buf->bo = bo_alloc;
		buf->dma_fd = bo_handle_fd.u32;
		buf->vaddr = bo_handle_vaddr.ptr;

		GST_DEBUG_OBJECT(camerasrc, "index %d buffer==> bo %p, dma_fd %d, vaddr %p",
				 i, buf->bo, buf->dma_fd, buf->vaddr);
	}
#endif

	return TRUE;
}

static void _destroy_buffer(GstCameraSrc *camerasrc)
{
#ifdef USE_NATIVE_DRM_BUFFER
	int i;

	for (i = 0; i < camerasrc->buffer_count; i++) {
		close(camerasrc->dma_fds[i]);
		close(camerasrc->gem_fds[i]);
		camerasrc->dma_fds[i] = -1;
		camerasrc->gem_fds[i] = -1;
	}
#else
	int i;
	camerasrc_buffer_t *buf;
	tbm_bufmgr bufmgr;
	for (i = 0; i < camerasrc->buffer_count; i++) {
		buf = &camerasrc->buffer[i];

		close(buf->dma_fd);
		tbm_bo_unref(buf->bo);

		buf->dma_fd = -1;
		buf->bo = NULL;
		buf->vaddr = NULL;
	}
#endif
}

static gboolean _start_preview(GstCameraSrc *camerasrc)
{
	int ret;
	int i;

	ret = nx_v4l2_reqbuf(camerasrc->clipper_video_fd, nx_clipper_video,
			     camerasrc->buffer_count);
	if (ret) {
		GST_ERROR_OBJECT(camerasrc, "failed to reqbuf");
		return FALSE;
	}

	for (i = 0; i < camerasrc->buffer_count; i++) {
#ifdef USE_NATIVE_DRM_BUFFER
		ret = nx_v4l2_qbuf(camerasrc->clipper_video_fd,
				   nx_clipper_video, 1, i,
				   &camerasrc->dma_fds[i],
				   (int *)&camerasrc->buffer_size);
#else
		camerasrc_buffer_t *buf = &camerasrc->buffer[i];
		ret = nx_v4l2_qbuf(camerasrc->clipper_video_fd,
				   nx_clipper_video, 1, i,
				   &buf->dma_fd,
				   (int *)&camerasrc->buffer_size);
#endif
		if (ret) {
			GST_ERROR_OBJECT(camerasrc, "failed to qbuf: index %d",
					 i);
			return FALSE;
		}
	}

	ret = nx_v4l2_streamon(camerasrc->clipper_video_fd, nx_clipper_video);
	if (ret) {
		GST_ERROR_OBJECT(camerasrc, "failed to streamon");
		return FALSE;
	}

	return TRUE;
}

static void _stop_preview(GstCameraSrc *camerasrc)
{
	nx_v4l2_streamoff(camerasrc->clipper_video_fd, nx_clipper_video);
	nx_v4l2_reqbuf(camerasrc->clipper_video_fd, nx_clipper_video, 0);
}

static gboolean _camera_start(GstCameraSrc *camerasrc)
{
	int sensor_fd;
	int clipper_subdev_fd;
	int csi_subdev_fd;
	int clipper_video_fd;
	gboolean is_mipi;
	guint32 module;
	int ret;
	guint32 bus_format;
	gboolean result;

	module = camerasrc->module;

	result = _get_frame_size(camerasrc->pixel_format, camerasrc->width,
				 camerasrc->height, &camerasrc->buffer_size);
	if (result == FALSE) {
		GST_ERROR_OBJECT(camerasrc, "failed to get frame_size");
		return result;
	}

	GST_INFO_OBJECT(camerasrc, "buffer size --> %d",
			camerasrc->buffer_size);

	/* open devices */
	sensor_fd = nx_v4l2_open_device(nx_sensor_subdev, module);
	if (sensor_fd < 0) {
		GST_ERROR_OBJECT(camerasrc, "failed to open sensor");
		return FALSE;
	}

	clipper_subdev_fd = nx_v4l2_open_device(nx_clipper_subdev, module);
	if (clipper_subdev_fd < 0) {
		GST_ERROR_OBJECT(camerasrc, "failed to open clipper_subdev");
		return FALSE;
	}

	clipper_video_fd = nx_v4l2_open_device(nx_clipper_video, module);
	if (clipper_video_fd < 0) {
		GST_ERROR_OBJECT(camerasrc, "failed to open clipper_video");
		return FALSE;
	}

	is_mipi = nx_v4l2_is_mipi_camera(module);
	if (is_mipi) {
		csi_subdev_fd = nx_v4l2_open_device(nx_csi_subdev, module);
		if (csi_subdev_fd < 0) {
			GST_ERROR_OBJECT(camerasrc, "failed to open mipi csi");
			return FALSE;
		}
	}

	/* link */
	ret = nx_v4l2_link(true, module, nx_clipper_subdev, 1,
			   nx_clipper_video, 0);
	if (ret) {
		GST_ERROR_OBJECT(camerasrc,
				 "failed to link clipper_sub -> clipper_video");
		return FALSE;
	}

	if (is_mipi) {
		ret = nx_v4l2_link(true, module, nx_sensor_subdev, 0,
				   nx_csi_subdev, 0);
		if (ret) {
			GST_ERROR_OBJECT(camerasrc,
					 "failed to link sensor -> mipi_csi");
			return FALSE;
		}

		ret = nx_v4l2_link(true, module, nx_csi_subdev, 1,
				   nx_clipper_subdev, 0);
		if (ret) {
			GST_ERROR_OBJECT(camerasrc,
					 "failed to link mipi_csi -> clipper_sub");
			return FALSE;
		}
	} else {
		ret = nx_v4l2_link(true, module, nx_sensor_subdev, 0,
				   nx_clipper_subdev, 0);
		if (ret) {
			GST_ERROR_OBJECT(camerasrc,
					 "failed to link sensor -> clipper_sub");
			return FALSE;
		}
	}

	/* FIXME: why following format is right format? */
	bus_format = MEDIA_BUS_FMT_YUYV8_2X8;
	ret = nx_v4l2_set_format(sensor_fd, nx_sensor_subdev, camerasrc->width,
				 camerasrc->height, bus_format);
	if (ret) {
		GST_ERROR_OBJECT(camerasrc,
				 "failed to set_format for sensor");
		return FALSE;
	}

	if (is_mipi) {
		ret = nx_v4l2_set_format(csi_subdev_fd, nx_csi_subdev,
					 camerasrc->width, camerasrc->height,
					 camerasrc->pixel_format);
		if (ret) {
			GST_ERROR_OBJECT(camerasrc,
					 "failed to set_format for mipi_csi");
			return FALSE;
		}
	}

	ret = nx_v4l2_set_format(clipper_subdev_fd, nx_clipper_subdev,
				 camerasrc->width, camerasrc->height,
				 bus_format);
	if (ret) {
		GST_ERROR_OBJECT(camerasrc,
				 "failed to set_format for clipper_subdev");
		return FALSE;
	}

	ret = nx_v4l2_set_format(clipper_video_fd, nx_clipper_video,
				 camerasrc->width, camerasrc->height,
				 camerasrc->pixel_format);
	if (ret) {
		GST_ERROR_OBJECT(camerasrc,
				 "failed to set_format for clipper_video");
		return FALSE;
	}

	if (camerasrc->crop_x && camerasrc->crop_y && camerasrc->crop_width
	    && camerasrc->crop_height) {
		ret = nx_v4l2_set_crop(clipper_subdev_fd, nx_clipper_subdev,
				       camerasrc->crop_x, camerasrc->crop_y,
				       camerasrc->crop_width,
				       camerasrc->crop_height);
		if (ret) {
			GST_ERROR_OBJECT(camerasrc,
					 "failed to set_crop for clipper_subdev");
			return FALSE;
		}
	} else {
		ret = nx_v4l2_set_crop(clipper_subdev_fd, nx_clipper_subdev,
				       0, 0,
				       camerasrc->width, camerasrc->height);
		if (ret) {
			GST_ERROR_OBJECT(camerasrc,
					 "failed to set_crop for clipper_subdev");
			return FALSE;
		}
	}

	camerasrc->sensor_fd = sensor_fd;
	camerasrc->clipper_subdev_fd = clipper_subdev_fd;
	if (is_mipi)
		camerasrc->csi_subdev_fd = csi_subdev_fd;
	camerasrc->clipper_video_fd = clipper_video_fd;

	camerasrc->is_mipi = is_mipi;

	camerasrc->buffer_count = DEF_BUFFER_COUNT;

	result = _create_buffer(camerasrc);
	if (result == FALSE) {
		GST_ERROR_OBJECT(camerasrc, "failed to create buffer");
		return FALSE;
	}

	return _start_preview(camerasrc);
}

static gboolean _camera_stop(GstCameraSrc *camerasrc)
{
	_stop_preview(camerasrc);
	_destroy_buffer(camerasrc);
	return TRUE;
}

static GstCaps *_find_intersect_caps(GstCaps *thiscaps, GstCaps *peercaps)
{
	int i;
	GstCaps *icaps = NULL;

	for (i = 0; i < gst_caps_get_size(peercaps); i++) {
		GstCaps *ipcaps = gst_caps_copy_nth(peercaps, i);
		icaps = gst_caps_intersect(thiscaps, ipcaps);
		gst_caps_unref(ipcaps);

		if (!gst_caps_is_empty(icaps))
			return icaps;

		gst_caps_unref(icaps);
	}

	return NULL;
}

static GstCaps *_find_best_match_caps(GstCaps *peercaps, GstCaps *icaps)
{
	GstCaps *caps;

	caps = icaps;
	if (gst_caps_get_size(icaps) > 1) {
		GstStructure *s;
		int best;
		int tw, th;

		s = gst_caps_get_structure(peercaps, 0);
		best = 0;

		if (gst_structure_get_int(s, "width", &tw) &&
		    gst_structure_get_int(s, "height", &th)) {
			int i;
			int w, h, maxw, maxh;
			GstStructure *is;

			maxw = G_MAXINT;
			maxh = G_MAXINT;

			for (i = gst_caps_get_size(icaps) - 1; i >= 0; i--) {
				is = gst_caps_get_structure(icaps, i);

				if (gst_structure_get_int(is, "width", &w) &&
				    gst_structure_get_int(is, "height", &h)) {
					if (w >= tw && w <= maxw &&
					    h >= th && h <= maxh) {
						maxw = w;
						maxh = h;
						best = i;
					}
				}
			}
		}

		caps = gst_caps_copy_nth(icaps, best);
		gst_caps_unref(icaps);
	}

	return caps;
}

static gboolean _get_timeinfo(GstCameraSrc *camerasrc, GstBuffer *buffer)
{
	GstClock *clock;
	GstClockTime timestamp;
	GstClockTime duration;

	timestamp = GST_CLOCK_TIME_NONE;
	duration = GST_CLOCK_TIME_NONE;

	clock = GST_ELEMENT_CLOCK(camerasrc);
	if (clock) {
		gst_object_ref(clock);
		timestamp = gst_clock_get_time(clock) -
			GST_ELEMENT(camerasrc)->base_time;
		gst_object_unref(clock);

		if (camerasrc->fps > 0)
			duration = gst_util_uint64_scale_int(GST_SECOND, 1,
							     camerasrc->fps); 
		else
			duration = gst_util_uint64_scale_int(GST_SECOND, 1, 30);
	}

	GST_BUFFER_TIMESTAMP(buffer) = timestamp;
	GST_BUFFER_DURATION(buffer) = duration;

	return TRUE;
}

#ifdef FOLLOWING_SAMSUNG_SCHEME

static GstCameraBuffer *_camerasrc_buffer_new(GstCameraSrc *camerasrc)
{
	GstCameraBuffer *buffer = NULL;

	buffer = (GstCameraBuffer *)malloc(sizeof(*buffer));
	buffer->buffer = gst_buffer_new();
	buffer->camerasrc = gst_object_ref(GST_OBJECT(camerasrc));

	return buffer;
}

static void gst_camerasrc_buffer_finalize(GstCameraBuffer *buffer);

static GstFlowReturn _read_preview(GstCameraSrc *camerasrc, GstBuffer **buffer)
{
	int ret;
	int v4l2_buffer_index;
	GstCameraBuffer *vid_buf = NULL;
	GstMemory *mem_camerabuf = NULL;

	GST_DEBUG_OBJECT(camerasrc, "camerasrc dequeue buffer");
	ret = nx_v4l2_dqbuf(camerasrc->clipper_video_fd, nx_clipper_video, 1,
			    &v4l2_buffer_index);
	if (ret) {
		GST_ERROR_OBJECT(camerasrc, "dq error");
		return GST_FLOW_ERROR;
	}

	vid_buf = _camerasrc_buffer_new(camerasrc);
	vid_buf->v4l2_buffer_index = v4l2_buffer_index;
	_get_timeinfo(camerasrc, vid_buf->buffer);

	mem_camerabuf = gst_memory_new_wrapped(GST_MEMORY_FLAG_NOT_MAPPABLE,
					       vid_buf,
					       sizeof(*vid_buf),
					       0,
					       sizeof(*vid_buf),
					       vid_buf,
				(GDestroyNotify)gst_camerasrc_buffer_finalize);
	if (mem_camerabuf) {
		gst_buffer_append_memory(vid_buf->buffer, mem_camerabuf);
	} else {
		GST_ERROR_OBJECT(camerasrc, "failed to create mem_camerabuf");
		goto ERROR;
	}

	*buffer = vid_buf->buffer;

	return GST_FLOW_OK;

ERROR:
	if (vid_buf) {
		gst_camerasrc_buffer_finalize(vid_buf);
		gst_buffer_unref((GstBuffer *)vid_buf);
		vid_buf = NULL;
	}
	
	return GST_FLOW_ERROR;
}
#endif

/* gobject_class methods */
static void gst_camerasrc_set_property(GObject *object, guint prop_id,
				       const GValue *value, GParamSpec *pspec)
{
	GstCameraSrc *camerasrc = NULL;

	g_return_if_fail(GST_IS_CAMERASRC(object));
	camerasrc = GST_CAMERASRC(object);

	switch (prop_id) {
	case ARG_CAMERA_ID:
		camerasrc->module = g_value_get_uint(value);
		GST_INFO_OBJECT(camerasrc, "Set CAMERA_ID: %u",
				camerasrc->module);
		break;
	case ARG_CAMERA_CROP_X:
		camerasrc->crop_x = g_value_get_uint(value);
		GST_INFO_OBJECT(camerasrc, "Set CAMERA_CROP_X: %u",
				camerasrc->crop_x);
		break;
	case ARG_CAMERA_CROP_Y:
		camerasrc->crop_y = g_value_get_uint(value);
		GST_INFO_OBJECT(camerasrc, "Set CAMERA_CROP_Y: %u",
				camerasrc->crop_y);
		break;
	case ARG_CAMERA_CROP_WIDTH:
		camerasrc->crop_width = g_value_get_uint(value);
		camerasrc->width = camerasrc->crop_width;
		GST_INFO_OBJECT(camerasrc, "Set CAMERA_CROP_WIDTH: %u",
				camerasrc->crop_width);
		break;
	case ARG_CAMERA_CROP_HEIGHT:
		camerasrc->crop_height = g_value_get_uint(value);
		camerasrc->height = camerasrc->crop_height;
		GST_INFO_OBJECT(camerasrc, "Set CAMERA_CROP_HEIGHT: %u",
				camerasrc->crop_height);
		break;
	case ARG_CAMERA_CAPTURE_FOURCC:
		camerasrc->capture_fourcc = g_value_get_uint(value);
		GST_INFO_OBJECT(camerasrc, "Set CAMERA_CAPTURE_FOURCC: %u",
				camerasrc->capture_fourcc);
		break;
	case ARG_CAMERA_CAPTURE_WIDTH:
		camerasrc->capture_width = g_value_get_uint(value);
		GST_INFO_OBJECT(camerasrc, "Set CAMERA_CAPTURE_WIDTH: %u",
				camerasrc->capture_width);
		break;
	case ARG_CAMERA_CAPTURE_HEIGHT:
		camerasrc->capture_height = g_value_get_uint(value);
		GST_INFO_OBJECT(camerasrc, "Set CAMERA_CAPTURE_HEIGHT: %u",
				camerasrc->capture_height);
		break;
	case ARG_CAMERA_CAPTURE_INTERVAL:
		camerasrc->capture_interval = g_value_get_uint(value);
		GST_INFO_OBJECT(camerasrc, "Set CAMERA_CAPTURE_INTERVAL: %u",
				camerasrc->capture_interval);
		break;
	case ARG_CAMERA_CAPTURE_COUNT:
		camerasrc->capture_count = g_value_get_uint(value);
		GST_INFO_OBJECT(camerasrc, "Set CAMERA_CAPTURE_COUNT: %u",
				camerasrc->capture_count);
		break;
	case ARG_CAMERA_CAPTURE_JPG_QUALITY:
		camerasrc->capture_jpg_quality = g_value_get_uint(value);
		GST_INFO_OBJECT(camerasrc, "Set CAMERA_CAPTURE_JPG_QUALITY: %u",
				camerasrc->capture_jpg_quality);
		break;
	case ARG_CAMERA_CAPTURE_PROVIDE_EXIF:
		camerasrc->capture_provide_exif = g_value_get_boolean(value);
		GST_INFO_OBJECT(camerasrc, "Set CAMERA_CAPTURE_PROVIDE_EXIF: %d"
				, camerasrc->capture_provide_exif);
	case ARG_VFLIP:
		camerasrc->vflip = g_value_get_boolean(value);
		GST_INFO_OBJECT(camerasrc, "Set VFLIP: %d", camerasrc->vflip);
		break;
	case ARG_HFLIP:
		camerasrc->hflip = g_value_get_boolean(value);
		GST_INFO_OBJECT(camerasrc, "Set HFLIP: %d", camerasrc->hflip);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void gst_camerasrc_get_property(GObject *object, guint prop_id,
				       GValue *value, GParamSpec *pspec)
{
	GstCameraSrc *camerasrc = NULL;

	g_return_if_fail(GST_IS_CAMERASRC(object));
	camerasrc = GST_CAMERASRC(object);

	switch (prop_id) {
	case ARG_CAMERA_ID:
		g_value_set_uint(value, camerasrc->module);
		break;
	case ARG_CAMERA_CROP_X:
		g_value_set_uint(value, camerasrc->crop_x);
		break;
	case ARG_CAMERA_CROP_Y:
		g_value_set_uint(value, camerasrc->crop_y);
		break;
	case ARG_CAMERA_CROP_WIDTH:
		g_value_set_uint(value, camerasrc->crop_width);
		break;
	case ARG_CAMERA_CROP_HEIGHT:
		g_value_set_uint(value, camerasrc->crop_height);
		break;
	case ARG_CAMERA_CAPTURE_FOURCC:
		g_value_set_uint(value, camerasrc->capture_fourcc);
		break;
	case ARG_CAMERA_CAPTURE_WIDTH:
		g_value_set_uint(value, camerasrc->capture_width);
		break;
	case ARG_CAMERA_CAPTURE_HEIGHT:
		g_value_set_uint(value, camerasrc->capture_height);
		break;
	case ARG_CAMERA_CAPTURE_INTERVAL:
		g_value_set_uint(value, camerasrc->capture_interval);
		break;
	case ARG_CAMERA_CAPTURE_COUNT:
		g_value_set_uint(value, camerasrc->capture_count);
		break;
	case ARG_CAMERA_CAPTURE_JPG_QUALITY:
		g_value_set_uint(value, camerasrc->capture_jpg_quality);
		break;
	case ARG_CAMERA_CAPTURE_PROVIDE_EXIF:
		g_value_set_boolean(value, camerasrc->capture_provide_exif);
		break;
	case ARG_VFLIP:
		g_value_set_boolean(value, camerasrc->vflip);
		break;
	case ARG_HFLIP:
		g_value_set_boolean(value, camerasrc->hflip);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void gst_camerasrc_finalize(GObject *object)
{
	/* TODO */
}

/* basesrc_class methods */
static gboolean gst_camerasrc_start(GstBaseSrc *src)
{
	/* opening the resource */
	GstCameraSrc *camerasrc;

	camerasrc = GST_CAMERASRC(src);

	GST_DEBUG_OBJECT(camerasrc, "ENTERED");

#ifndef FOLLOWING_SAMSUNG_SCHEME 
	return _camera_start(camerasrc);
#else
	GST_DEBUG_OBJECT(camerasrc, "LEAVED");

	return TRUE;
#endif
}

static gboolean gst_camerasrc_stop(GstBaseSrc *src)
{
	GstCameraSrc *camerasrc;

	camerasrc = GST_CAMERASRC(src);

	GST_DEBUG_OBJECT(camerasrc, "ENTERED");

	_camera_stop(camerasrc);

	GST_DEBUG_OBJECT(camerasrc, "LEAVED");

	return TRUE;
}

static GstCaps *gst_camerasrc_get_caps(GstBaseSrc *src, GstCaps *filter)
{
	GstCameraSrc *camerasrc;
	GstCaps *caps = NULL;

	camerasrc = GST_CAMERASRC(src);

	GST_DEBUG_OBJECT(camerasrc, "ENTERED");

	caps = gst_caps_copy(gst_pad_get_pad_template_caps(GST_BASE_SRC_PAD
							   (camerasrc)));
	if (caps && filter)
		gst_caps_take(&caps, gst_caps_intersect(caps, filter));

	GST_INFO_OBJECT(camerasrc, "probed caps: %p", caps);
	GST_DEBUG_OBJECT(camerasrc, "LEAVED");

	return caps;
}

static gboolean gst_camerasrc_set_caps(GstBaseSrc *src, GstCaps *caps)
{
	GstCameraSrc *camerasrc;
	guint size;
	gboolean ret;

	camerasrc = GST_CAMERASRC(src);

	GST_DEBUG_OBJECT(camerasrc, "ENTERED");

	if (!_get_caps_info(camerasrc, caps, &size)) {
		GST_ERROR_OBJECT(camerasrc, "can't get capture information from caps %p", caps);
		return FALSE;
	}

	camerasrc->buffer_size = size;

#ifdef FOLLOWING_SAMSUNG_SCHEME
	if (!_camera_start(camerasrc)) {
		GST_ERROR_OBJECT(camerasrc, "failed to camera start");
		return FALSE;
	}

	ret = gst_pad_push_event(GST_BASE_SRC_PAD(src),
				 gst_event_new_caps(caps));

	GST_INFO_OBJECT(camerasrc, "LEAVED");

	return ret;
#else
	GST_INFO_OBJECT(camerasrc, "LEAVED");

	return TRUE;
#endif
}

static gboolean gst_camerasrc_negotiate(GstBaseSrc *src)
{
	GstCaps *thiscaps;
	GstCaps *caps;
	GstCaps *peercaps;
	gboolean ret;
	GstCameraSrc *camerasrc;

	camerasrc = GST_CAMERASRC(src);

	GST_DEBUG_OBJECT(camerasrc, "ENTERED");

	thiscaps = gst_pad_query_caps(GST_BASE_SRC_PAD(src), NULL);
	GST_DEBUG_OBJECT(src, "caps of src: %" GST_PTR_FORMAT, thiscaps);

	if (thiscaps == NULL || gst_caps_is_any(thiscaps))
		return TRUE;

	peercaps = gst_pad_peer_query_caps(GST_BASE_SRC_PAD(src), NULL);
	GST_DEBUG_OBJECT(src, "caps of peer: %" GST_PTR_FORMAT, peercaps);

	caps = NULL;
	if (peercaps && !gst_caps_is_any(peercaps)) {
		GstCaps *icaps = _find_intersect_caps(thiscaps, peercaps);
		GST_DEBUG_OBJECT(src, "intersect: %" GST_PTR_FORMAT,
				 icaps);
		if (icaps)
			caps = _find_best_match_caps(peercaps, icaps);
	} else {
		caps = thiscaps;
	}

	ret = FALSE;
	if (caps) {
		caps = gst_caps_fixate(caps);

		if (!gst_caps_is_empty(caps)) {
			GST_DEBUG_OBJECT(src, "fixated to: %"
					 GST_PTR_FORMAT, caps);

			if (gst_caps_is_any(caps))
				ret = TRUE;
			else if (gst_caps_is_fixed(caps))
				ret = gst_camerasrc_set_caps(src, caps);
		}
		gst_caps_unref(caps);
	}

	GST_DEBUG_OBJECT(camerasrc, "LEAVED");

	return ret;
}

/* pushsrc_class methods */
static GstFlowReturn gst_camerasrc_create(GstPushSrc *src,
					  GstBuffer **buffer)
{
	GstCameraSrc *camerasrc;
	GstFlowReturn ret;

	camerasrc = GST_CAMERASRC(src);

	GST_DEBUG_OBJECT(camerasrc, "ENTERED");

	ret = _read_preview(camerasrc, buffer);

	GST_DEBUG_OBJECT(camerasrc, "LEAVED");

	return ret;
}

/* URI handler */
static GstURIType gst_camerasrc_uri_get_type(GType type)
{
	return GST_URI_SRC;
}

static const gchar *const *gst_camerasrc_uri_get_protocols(GType type)
{
	static const gchar *protocols[] = { "nxcamera", NULL};
	return protocols;
}

static gchar *gst_camerasrc_uri_get_uri(GstURIHandler *handler)
{
	GstCameraSrc *camerasrc;

	camerasrc = GST_CAMERASRC(handler);

	return g_strdup_printf("nxcamera://%d", camerasrc->module);
}

static gboolean gst_camerasrc_uri_set_uri(GstURIHandler *handler,
					  const gchar *uri,
					  GError **error)
{
	GstCameraSrc *camerasrc;

	camerasrc = GST_CAMERASRC(handler);

	if (strcmp(uri, "nxcamera://")) {
		const gchar *module = uri + 11;;
		g_object_set(camerasrc, "camera-id", atoi(module), NULL);
	}

	return TRUE;

}

static void gst_camerasrc_uri_handler_init(gpointer g_iface,
					   gpointer iface_data)
{
	GstURIHandlerInterface *iface = (GstURIHandlerInterface *)g_iface;

	iface->get_type = gst_camerasrc_uri_get_type;
	iface->get_protocols = gst_camerasrc_uri_get_protocols;
	iface->get_uri = gst_camerasrc_uri_get_uri;
	iface->set_uri = gst_camerasrc_uri_set_uri;
}

G_DEFINE_TYPE_WITH_CODE(GstCameraSrc, gst_camerasrc, GST_TYPE_PUSH_SRC,
			G_IMPLEMENT_INTERFACE(GST_TYPE_URI_HANDLER,
				gst_camerasrc_uri_handler_init));

static void gst_camerasrc_class_init(GstCameraSrcClass *klass)
{
	GST_DEBUG_CATEGORY_INIT(camerasrc_debug, "camerasrc", 0, "camerasrc element");

	GObjectClass *gobject_class;
	GstElementClass *element_class;
	GstBaseSrcClass *basesrc_class;
	GstPushSrcClass *pushsrc_class;

	GST_DEBUG("ENTERED");

	gobject_class = G_OBJECT_CLASS(klass);
	element_class = GST_ELEMENT_CLASS(klass);
	basesrc_class = GST_BASE_SRC_CLASS(klass);
	pushsrc_class = GST_PUSH_SRC_CLASS(klass);

	/* gobject_class overriding */
	gobject_class->set_property = gst_camerasrc_set_property;
	gobject_class->get_property = gst_camerasrc_get_property;
	gobject_class->finalize = gst_camerasrc_finalize;

	/* gobject property */
	g_object_class_install_property(gobject_class, ARG_CAMERA_ID,
					g_param_spec_uint("camera-id",
							  "index number of camera to activate",
							  "index number of camera to activate",
							  MIN_CAMERA_ID,
							  MAX_CAMERA_ID,
							  0,
							  G_PARAM_READWRITE));
	g_object_class_install_property(gobject_class, ARG_CAMERA_CROP_X,
					g_param_spec_uint("camera-crop-x",
							  "Crop X",
							  "X value for crop",
							  0,
							  MAX_RESOLUTION_X,
							  0,
							  G_PARAM_READWRITE));
	g_object_class_install_property(gobject_class, ARG_CAMERA_CROP_Y,
					g_param_spec_uint("camera-crop-y",
							  "Crop Y",
							  "Y value for crop",
							  0,
							  MAX_RESOLUTION_Y,
							  0,
							  G_PARAM_READWRITE));
	g_object_class_install_property(gobject_class, ARG_CAMERA_CROP_WIDTH,
					g_param_spec_uint("camera-crop-width",
							  "Crop WIDTH",
							  "WIDTH value for crop",
							  0,
							  MAX_RESOLUTION_X,
							  0,
							  G_PARAM_READWRITE));
	g_object_class_install_property(gobject_class, ARG_CAMERA_CROP_HEIGHT,
					g_param_spec_uint("camera-crop-height",
							  "Crop HEIGHT",
							  "HEIGHT value for crop",
							  0,
							  MAX_RESOLUTION_Y,
							  0,
							  G_PARAM_READWRITE));
	g_object_class_install_property(gobject_class,
					ARG_CAMERA_CAPTURE_FOURCC,
					g_param_spec_uint("capture-fourcc",
							  "Capture format",
							  "Fourcc value for capture format",
							  0,
							  G_MAXUINT,
							  0,
							  G_PARAM_READWRITE));
	g_object_class_install_property(gobject_class,
					ARG_CAMERA_CAPTURE_WIDTH,
					g_param_spec_uint("capture-width",
							  "Capture width",
							  "Width for camera size to capture",
							  0,
							  G_MAXUINT,
							  DEF_CAPTURE_WIDTH,
							  G_PARAM_READWRITE |
							  G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(gobject_class,
					ARG_CAMERA_CAPTURE_HEIGHT,
					g_param_spec_uint("capture-height",
							  "Capture height",
							  "Height for camera size to capture",
							  0,
							  G_MAXUINT,
							  DEF_CAPTURE_HEIGHT,
							  G_PARAM_READWRITE |
							  G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(gobject_class,
					ARG_CAMERA_CAPTURE_INTERVAL,
					g_param_spec_uint("capture-interval",
							  "Capture interval",
							  "Interval time to capture (millisecond)",
							  0,
							  G_MAXUINT,
							  DEF_CAPTURE_INTERVAL,
							  G_PARAM_READWRITE |
							  G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(gobject_class,
					ARG_CAMERA_CAPTURE_COUNT,
					g_param_spec_uint("capture-count",
							  "Capture count",
							  "Capture count for multishot",
							  1,
							  G_MAXUINT,
							  DEF_CAPTURE_COUNT,
							  G_PARAM_READWRITE |
							  G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(gobject_class,
					ARG_CAMERA_CAPTURE_JPG_QUALITY,
					g_param_spec_uint("capture-jpg-quality",
							  "JPEG Capture compress ratio",
							  "Quality of capture image compress ratio",
							  1,
							  100,
							  DEF_CAPTURE_JPG_QUALITY,
							  G_PARAM_READWRITE |
							  G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(gobject_class,
					ARG_CAMERA_CAPTURE_PROVIDE_EXIF,
					g_param_spec_boolean("provide-exif",
							     "Whether EXIF is provided",
							     "Does capture provide EXIF?",
							     DEF_CAPTURE_PROVIDE_EXIF,
							     G_PARAM_READWRITE |
							     G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(gobject_class,
					ARG_VFLIP,
					g_param_spec_boolean("vflip",
							     "Flip vertically",
							     "Flip camera input vertically",
							     0,
							     G_PARAM_READWRITE |
							     G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(gobject_class,
					ARG_HFLIP,
					g_param_spec_boolean("hflip",
							     "Flip horizontally",
							     "Flip camera input horizontally",
							     0,
							     G_PARAM_READWRITE |
							     G_PARAM_STATIC_STRINGS));

	/* element_class overriding */
	gst_element_class_add_pad_template(element_class,
				gst_static_pad_template_get(&src_factory));
	gst_element_class_set_static_metadata(element_class,
					      "Nexell Camera Source GStreamer Plug-in",
					      "Source/Video",
					      "camera src for videosrc based GStreamer Plug-in",
					      "Sungwoo Park <swpark@nexell.co.kr>");
	
	/* basesrc_class overriding */
	basesrc_class->start = gst_camerasrc_start;
	basesrc_class->stop = gst_camerasrc_stop;
	basesrc_class->get_caps = gst_camerasrc_get_caps;
	basesrc_class->set_caps = gst_camerasrc_set_caps;
	basesrc_class->negotiate = gst_camerasrc_negotiate;

	/* pushsrc_class overriding */
#ifdef FOLLOWING_SAMSUNG_SCHEME
	pushsrc_class->create = gst_camerasrc_create;
#endif

	GST_DEBUG("LEAVED");
}

static void gst_camerasrc_init(GstCameraSrc *camerasrc)
{
	int i;

	GST_DEBUG("ENTERED");

	camerasrc->module = 0;

	camerasrc->sensor_fd = -1;
	camerasrc->clipper_subdev_fd = -1;
	camerasrc->decimator_subdev_fd = -1;
	camerasrc->csi_subdev_fd = -1;
	camerasrc->clipper_video_fd = -1;
	camerasrc->decimator_video_fd = -1;

	camerasrc->is_mipi = FALSE;

	/* input attribute */
	camerasrc->width = DEF_CAPTURE_WIDTH;
	camerasrc->height = DEF_CAPTURE_HEIGHT;
	camerasrc->pixel_format = DEF_PIXEL_FORMAT;
	camerasrc->fps = DEF_FPS;

	/* crop attribute */
	camerasrc->crop_x = 0;
	camerasrc->crop_y = 0;
	camerasrc->crop_width = DEF_CAPTURE_WIDTH;
	camerasrc->crop_height = DEF_CAPTURE_HEIGHT;

	/* flip attribute */
	camerasrc->vflip = FALSE;
	camerasrc->hflip = FALSE;

	/* buffer */
	camerasrc->buffer_count = 0;
	camerasrc->buffer_size = 0;
#ifdef USE_NATIVE_DRM_BUFFER
	camerasrc->drm_fd = -1;
	for (i = 0; i < MAX_BUFFER_COUNT; i++) {
		camerasrc->gem_fds[i] = -1;
		camerasrc->dma_fds[i] = -1;
	}
#else
	camerasrc->bufmgr = NULL;
	for (i = 0; i < MAX_BUFFER_COUNT; i++) {
		memset(&camerasrc->buffer[i], 0, sizeof(camerasrc->buffer));
	}
#endif

	gst_base_src_set_format(GST_BASE_SRC(camerasrc), GST_FORMAT_TIME);
	gst_base_src_set_live(GST_BASE_SRC(camerasrc), TRUE);
	gst_base_src_set_do_timestamp(GST_BASE_SRC(camerasrc), TRUE);

	GST_DEBUG("LEAVED");
}

/**
 * GstCameraBuffer
 */
static void gst_camerasrc_buffer_finalize(GstCameraBuffer *buffer)
{
	/* TODO */
	int index;
	int ret;
	GstCameraSrc *camerasrc;

	camerasrc = buffer->camerasrc;
	index = buffer->v4l2_buffer_index;

	GST_DEBUG_OBJECT(camerasrc, "ENTERED");

	GST_DEBUG_OBJECT(camerasrc, "buffer index: %d", index);

	ret = nx_v4l2_qbuf(camerasrc->clipper_video_fd, nx_clipper_video, 1,
			   index,
			   &camerasrc->dma_fds[index],
			   (int *)&camerasrc->buffer_size);
	if (ret)
		GST_ERROR_OBJECT(camerasrc, "q error");

	GST_DEBUG_OBJECT(camerasrc, "LEAVED");
}

G_DEFINE_BOXED_TYPE(GstCameraBuffer, gst_camerasrc_buffer, NULL,
		    gst_camerasrc_buffer_finalize);

static gboolean plugin_init(GstPlugin *plugin)
{
	return gst_element_register(plugin, "camerasrc", GST_RANK_PRIMARY + 100,
				    GST_TYPE_CAMERASRC);
}

#ifndef PACKAGE
#define PACKAGE "nexell.gst.camera"
#endif

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR,
		  GST_VERSION_MINOR,
		  camerasrc,
		  "Camera source plug-in",
		  plugin_init,
		  "0.0.1",
		  "LGPL",
		  "Nexell Co",
		  "http://www.nexell.co.kr")