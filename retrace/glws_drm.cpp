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

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "glws_drm.hpp"

#define VOID2U64(x) ((uint64_t)(unsigned long)(x))

static struct gbm gbm;
static struct drm drm = {
    .kms_out_fence_fd = -1,
};

static void
drm_fb_destroy_callback(struct gbm_bo *bo, void *data) {
    int drm_fd = gbm_device_get_fd(gbm_bo_get_device(bo));
    struct drm_fb *fb = (struct drm_fb *)data;

    if (fb->fb_id)
        drmModeRmFB(drm_fd, fb->fb_id);

    free(fb);
}

struct drm_fb *
drm_fb_get_from_bo(struct gbm_bo *bo) {
    int drm_fd = gbm_device_get_fd(gbm_bo_get_device(bo));
    struct drm_fb *fb = (struct drm_fb *)gbm_bo_get_user_data(bo);
    uint32_t width, height, format,
        strides[4] = { 0 }, handles[4] = { 0 },
        offsets[4] = { 0 }, flags = 0;
    int ret = -1;

    if (fb)
        return fb;

    fb = (struct drm_fb *)calloc(1, sizeof *fb);
    fb->bo = bo;

    width = gbm_bo_get_width(bo);
    height = gbm_bo_get_height(bo);
    format = gbm_bo_get_format(bo);

    uint64_t modifiers[4] = { 0 };
    modifiers[0] = gbm_bo_get_modifier(bo);
    const int num_planes = gbm_bo_get_plane_count(bo);
    for (int i = 0; i < num_planes; i++) {
        handles[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
        strides[i] = gbm_bo_get_stride_for_plane(bo, i);
        offsets[i] = gbm_bo_get_offset(bo, i);
        modifiers[i] = modifiers[0];
    }

    if (modifiers[0]) {
        flags = DRM_MODE_FB_MODIFIERS;
        printf("Using modifier %" PRIx64 "\n", modifiers[0]);
    }

    ret = drmModeAddFB2WithModifiers(drm_fd, width, height,
                                     format, handles, strides, offsets,
                                     modifiers, &fb->fb_id, flags);


    if (ret) {
        if (flags)
            fprintf(stderr, "Modifiers failed!\n");

        uint32_t temp_handles[4] = { gbm_bo_get_handle(bo).u32, 0, 0, 0 };
        uint32_t temp_strides[4] = { gbm_bo_get_stride(bo), 0, 0, 0 };
        memcpy(handles, temp_handles, 16);
        memcpy(strides, temp_strides, 16);
        memset(offsets, 0, 16);
        ret = drmModeAddFB2(drm_fd, width, height, format,
                            handles, strides, offsets, &fb->fb_id, 0);
    }

    if (ret) {
        printf("failed to create fb: %s\n", strerror(errno));
        free(fb);
        return NULL;
    }

    gbm_bo_set_user_data(bo, fb, drm_fb_destroy_callback);

    return fb;
}

static uint32_t
find_crtc_for_encoder(const drmModeRes *resources,
                      const drmModeEncoder *encoder) {
    int i;

    for (i = 0; i < resources->count_crtcs; i++) {
        /* possible_crtcs is a bitmask as described here:
         * https://dvdhrm.wordpress.com/2012/09/13/linux-drm-mode-setting-api
         */
        const uint32_t crtc_mask = 1 << i;
        const uint32_t crtc_id = resources->crtcs[i];
        if (encoder->possible_crtcs & crtc_mask) {
            return crtc_id;
        }
    }

    /* no match found */
    return -1;
}

static uint32_t
find_crtc_for_connector(const struct drm *drm, const drmModeRes *resources,
                        const drmModeConnector *connector) {
    int i;

    for (i = 0; i < connector->count_encoders; i++) {
        const uint32_t encoder_id = connector->encoders[i];
        drmModeEncoder *encoder = drmModeGetEncoder(drm->fd, encoder_id);

        if (encoder) {
            const uint32_t crtc_id = find_crtc_for_encoder(resources, encoder);

            drmModeFreeEncoder(encoder);
            if (crtc_id != 0) {
                return crtc_id;
            }
        }
    }

    /* no match found */
    return -1;
}

static int
get_resources(int fd, drmModeRes **resources) {
    *resources = drmModeGetResources(fd);
    if (*resources == NULL)
        return -1;
    return 0;
}

#define MAX_DRM_DEVICES 64

static int
find_drm_device(drmModeRes **resources) {
    drmDevicePtr devices[MAX_DRM_DEVICES] = { NULL };
    int num_devices, fd = -1;

    num_devices = drmGetDevices2(0, devices, MAX_DRM_DEVICES);
    if (num_devices < 0) {
        printf("drmGetDevices2 failed: %s\n", strerror(-num_devices));
        return -1;
    }

    for (int i = 0; i < num_devices; i++) {
        drmDevicePtr device = devices[i];
        int ret;

        if (!(device->available_nodes & (1 << DRM_NODE_PRIMARY)))
            continue;
        /* OK, it's a primary device. If we can get the
         * drmModeResources, it means it's also a
         * KMS-capable device.
         */
        fd = open(device->nodes[DRM_NODE_PRIMARY], O_RDWR);
        if (fd < 0)
            continue;
        ret = get_resources(fd, resources);
        if (!ret)
            break;
        close(fd);
        fd = -1;
    }
    drmFreeDevices(devices, num_devices);

    if (fd < 0)
        printf("no drm device found!\n");
    return fd;
}

int
init_drm(struct drm *drm, const char *device, const char *mode_str,
         unsigned int vrefresh, unsigned int count) {
    drmModeRes *resources;
    drmModeConnector *connector = NULL;
    drmModeEncoder *encoder = NULL;
    int i, ret, area;

    if (device) {
        drm->fd = open(device, O_RDWR);
        ret = get_resources(drm->fd, &resources);
        if (ret < 0 && errno == EOPNOTSUPP)
            printf("%s does not look like a modeset device\n", device);
    } else {
        drm->fd = find_drm_device(&resources);
    }

    if (drm->fd < 0) {
        printf("could not open drm device\n");
        return -1;
    }

    if (!resources) {
        printf("drmModeGetResources failed: %s\n", strerror(errno));
        return -1;
    }

    /* find a connected connector: */
    for (i = 0; i < resources->count_connectors; i++) {
        connector = drmModeGetConnector(drm->fd, resources->connectors[i]);
        if (connector->connection == DRM_MODE_CONNECTED) {
            /* it's connected, let's use this! */
            break;
        }
        drmModeFreeConnector(connector);
        connector = NULL;
    }

    if (!connector) {
        /* we could be fancy and listen for hotplug events and wait for
         * a connector..
         */
        printf("no connected connector!\n");
        return -1;
    }

    /* find user requested mode: */
    if (mode_str && *mode_str) {
        for (i = 0; i < connector->count_modes; i++) {
            drmModeModeInfo *current_mode = &connector->modes[i];

            if (strcmp(current_mode->name, mode_str) == 0) {
                if (vrefresh == 0 || current_mode->vrefresh == vrefresh) {
                    drm->mode = current_mode;
                    break;
                }
            }
        }
        if (!drm->mode)
            printf("requested mode not found, using default mode!\n");
    }

    /* find preferred mode or the highest resolution mode: */
    if (!drm->mode) {
        for (i = 0, area = 0; i < connector->count_modes; i++) {
            drmModeModeInfo *current_mode = &connector->modes[i];

            if (current_mode->type & DRM_MODE_TYPE_PREFERRED) {
                drm->mode = current_mode;
                break;
            }

            int current_area = current_mode->hdisplay * current_mode->vdisplay;
            if (current_area > area) {
                drm->mode = current_mode;
                area = current_area;
            }
        }
    }

    if (!drm->mode) {
        printf("could not find mode!\n");
        return -1;
    }

    /* find encoder: */
    for (i = 0; i < resources->count_encoders; i++) {
        encoder = drmModeGetEncoder(drm->fd, resources->encoders[i]);
        if (encoder->encoder_id == connector->encoder_id)
            break;
        drmModeFreeEncoder(encoder);
        encoder = NULL;
    }

    if (encoder) {
        drm->crtc_id = encoder->crtc_id;
    } else {
        uint32_t crtc_id = find_crtc_for_connector(drm, resources, connector);
        if (crtc_id == 0) {
            printf("no crtc found!\n");
            return -1;
        }

        drm->crtc_id = crtc_id;
    }

    for (i = 0; i < resources->count_crtcs; i++) {
        if (resources->crtcs[i] == drm->crtc_id) {
            drm->crtc_index = i;
            break;
        }
    }

    drmModeFreeResources(resources);

    drm->connector_id = connector->connector_id;
    drm->count = count;

    return 0;
}

static struct gbm_bo *
init_bo(uint64_t modifier) {
    struct gbm_bo *bo = NULL;

    bo = gbm_bo_create_with_modifiers(gbm.dev,
                                      gbm.width, gbm.height,
                                      gbm.format,
                                      &modifier, 1);

    if (!bo) {
        if (modifier != DRM_FORMAT_MOD_LINEAR) {
            fprintf(stderr, "Modifiers requested but support isn't available\n");
            return NULL;
        }

        bo = gbm_bo_create(gbm.dev,
                           gbm.width, gbm.height,
                           gbm.format,
                           GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    }

    if (!bo) {
        printf("failed to create gbm bo\n");
        return NULL;
    }

    return bo;
}

static struct gbm *
init_surfaceless(uint64_t modifier) {
    for (unsigned i = 0; i < ARRAY_SIZE(gbm.bos); i++) {
        gbm.bos[i] = init_bo(modifier);
        if (!gbm.bos[i])
            return NULL;
    }
    return &gbm;
}

static struct gbm *
init_surface(uint64_t modifier) {
    gbm.surface = gbm_surface_create_with_modifiers(gbm.dev,
                                                    gbm.width, gbm.height,
                                                    gbm.format,
                                                    &modifier, 1);

    if (!gbm.surface) {
        if (modifier != DRM_FORMAT_MOD_LINEAR) {
            fprintf(stderr, "Modifiers requested but support isn't available\n");
            return NULL;
        }
        gbm.surface = gbm_surface_create(gbm.dev,
                                         gbm.width, gbm.height,
                                         gbm.format,
                                         GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    }

    if (!gbm.surface) {
        printf("failed to create gbm surface\n");
        return NULL;
    }

    return &gbm;
}

const struct gbm *
init_gbm(int drm_fd, int w, int h, uint32_t format,
         uint64_t modifier, bool surfaceless) {
    gbm.dev = gbm_create_device(drm_fd);
    gbm.format = format;
    gbm.surface = NULL;

    gbm.width = w;
    gbm.height = h;

    if (surfaceless)
        return init_surfaceless(modifier);

    return init_surface(modifier);
}

int64_t
get_time_ns(void) {
    struct timespec tv;
    clock_gettime(CLOCK_MONOTONIC, &tv);
    return tv.tv_nsec + tv.tv_sec * NSEC_PER_SEC;
}


/* Pick a plane.. something that at a minimum can be connected to
 * the chosen crtc, but prefer primary plane.
 *
 * Seems like there is some room for a drmModeObjectGetNamedProperty()
 * type helper in libdrm..
 */
static int
get_plane_id(void) {
    drmModePlaneResPtr plane_resources;
    uint32_t i, j;
    int ret = -EINVAL;
    int found_primary = 0;

    plane_resources = drmModeGetPlaneResources(drm.fd);
    if (!plane_resources) {
        printf("drmModeGetPlaneResources failed: %s\n", strerror(errno));
        return -1;
    }

    for (i = 0; (i < plane_resources->count_planes) && !found_primary; i++) {
        uint32_t id = plane_resources->planes[i];
        drmModePlanePtr plane = drmModeGetPlane(drm.fd, id);
        if (!plane) {
            printf("drmModeGetPlane(%u) failed: %s\n", id, strerror(errno));
            continue;
        }

        if (plane->possible_crtcs & (1 << drm.crtc_index)) {
            drmModeObjectPropertiesPtr props =
                drmModeObjectGetProperties(drm.fd, id, DRM_MODE_OBJECT_PLANE);

            /* primary or not, this plane is good enough to use: */
            ret = id;

            for (j = 0; j < props->count_props; j++) {
                drmModePropertyPtr p =
                    drmModeGetProperty(drm.fd, props->props[j]);

                if ((strcmp(p->name, "type") == 0) &&
                    (props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY)) {
                    /* found our primary plane, lets use that: */
                    found_primary = 1;
                }

                drmModeFreeProperty(p);
            }

            drmModeFreeObjectProperties(props);
        }

        drmModeFreePlane(plane);
    }

    drmModeFreePlaneResources(plane_resources);

    return ret;
}

const struct drm *
init_drm_atomic(const char *device, const char *mode_str,
                unsigned int vrefresh, unsigned int count) {
    uint32_t plane_id;
    int ret;

    ret = init_drm(&drm, device, mode_str, vrefresh, count);
    if (ret)
        return NULL;

    ret = drmSetClientCap(drm.fd, DRM_CLIENT_CAP_ATOMIC, 1);
    if (ret) {
        printf("no atomic modesetting support: %s\n", strerror(errno));
        return NULL;
    }

    ret = get_plane_id();
    if (!ret) {
        printf("could not find a suitable plane\n");
        return NULL;
    } else {
        plane_id = ret;
    }

    /* We only do single plane to single crtc to single connector, no
     * fancy multi-monitor or multi-plane stuff.  So just grab the
     * plane/crtc/connector property info for one of each:
     */
    drm.plane = (struct plane *)calloc(1, sizeof(*drm.plane));
    drm.crtc = (struct crtc *)calloc(1, sizeof(*drm.crtc));
    drm.connector = (struct connector *)calloc(1, sizeof(*drm.connector));

#define get_resource(type, Type, id)                   \
    do {                                               \
        drm.type->type = drmModeGet##Type(drm.fd, id); \
        if (!drm.type->type) {                         \
            printf("could not get %s %i: %s\n",        \
                   #type, id, strerror(errno));        \
            return NULL;                               \
        }                                              \
    } while (0)

    get_resource(plane, Plane, plane_id);
    get_resource(crtc, Crtc, drm.crtc_id);
    get_resource(connector, Connector, drm.connector_id);

#define get_properties(type, TYPE, id)                                                       \
    do {                                                                                     \
        uint32_t i;                                                                          \
        drm.type->props = drmModeObjectGetProperties(drm.fd,                                 \
                                                     id, DRM_MODE_OBJECT_##TYPE);            \
        if (!drm.type->props) {                                                              \
            printf("could not get %s %u properties: %s\n",                                   \
                   #type, id, strerror(errno));                                              \
            return NULL;                                                                     \
        }                                                                                    \
        drm.type->props_info = (drmModePropertyRes **)calloc(drm.type->props->count_props,   \
                                                             sizeof(*drm.type->props_info)); \
        for (i = 0; i < drm.type->props->count_props; i++) {                                 \
            drm.type->props_info[i] = drmModeGetProperty(drm.fd,                             \
                                                         drm.type->props->props[i]);         \
        }                                                                                    \
    } while (0)

    get_properties(plane, PLANE, plane_id);
    get_properties(crtc, CRTC, drm.crtc_id);
    get_properties(connector, CONNECTOR, drm.connector_id);

    return &drm;
}
