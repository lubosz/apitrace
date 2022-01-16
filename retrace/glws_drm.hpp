/*
 * Copyright 2017 Rob Clark <rclark@redhat.com>
 * Copyright 2021 Lubosz Sarnecki <lubosz@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * DRM / GBM implementation based on drm-atomic.c from kmscube.
 */

#pragma once

#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define NUM_BUFFERS 2
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define NSEC_PER_SEC (INT64_C(1000) * USEC_PER_SEC)
#define USEC_PER_SEC (INT64_C(1000) * MSEC_PER_SEC)
#define MSEC_PER_SEC INT64_C(1000)

struct gbm {
    struct gbm_device *dev;
    struct gbm_surface *surface;
    struct gbm_bo *bos[NUM_BUFFERS]; /* for the surfaceless case */
    uint32_t format;
    int width, height;
};

struct plane {
    drmModePlane *plane;
    drmModeObjectProperties *props;
    drmModePropertyRes **props_info;
};

struct crtc {
    drmModeCrtc *crtc;
    drmModeObjectProperties *props;
    drmModePropertyRes **props_info;
};

struct connector {
    drmModeConnector *connector;
    drmModeObjectProperties *props;
    drmModePropertyRes **props_info;
};

struct drm {
    int fd;

    /* only used for atomic: */
    struct plane *plane;
    struct crtc *crtc;
    struct connector *connector;
    int crtc_index;
    int kms_in_fence_fd;
    int kms_out_fence_fd;

    drmModeModeInfo *mode;
    uint32_t crtc_id;
    uint32_t connector_id;

    /* number of frames to run for: */
    unsigned int count;
};

struct drm_fb {
    struct gbm_bo *bo;
    uint32_t fb_id;
};

const struct gbm *init_gbm(int drm_fd, int w, int h, uint32_t format, uint64_t modifier, bool surfaceless);
int init_drm(struct drm *drm, const char *device, const char *mode_str, unsigned int vrefresh, unsigned int count);
int64_t get_time_ns(void);
struct drm_fb *drm_fb_get_from_bo(struct gbm_bo *bo);
const struct drm *init_drm_atomic(const char *device, const char *mode_str, unsigned int vrefresh, unsigned int count);
