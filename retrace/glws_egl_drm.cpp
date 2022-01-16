/**************************************************************************
 *
 * Copyright 2011 LunarG, Inc.
 * Copyright 2011 Jose Fonseca
 * Copyright 2017 Rob Clark <rclark@redhat.com>
 * Copyright 2021 Lubosz Sarnecki <lubosz@gmail.com>
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 **************************************************************************/

/*
 * Based on glws_egl_xlib.cpp and drm-atomic. from kmscube.
 */

#include <assert.h>
#include <dlfcn.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <iostream>

#include "glproc.hpp"
#include "glws.hpp"
#include "glws_drm.hpp"

namespace glws {


static EGLDisplay eglDisplay = EGL_NO_DISPLAY;
static char const *eglExtensions = NULL;
static bool has_EGL_KHR_create_context = false;

static const struct drm *drm;
static const struct gbm *gbm;

static EGLenum
translateAPI(glfeatures::Profile profile) {
    switch (profile.api) {
    case glfeatures::API_GL:
        return EGL_OPENGL_API;
    case glfeatures::API_GLES:
        return EGL_OPENGL_ES_API;
    default:
        assert(0);
        return EGL_NONE;
    }
}


/* Must be called before
 *
 * - eglCreateContext
 * - eglGetCurrentContext
 * - eglGetCurrentDisplay
 * - eglGetCurrentSurface
 * - eglMakeCurrent (when its ctx parameter is EGL_NO_CONTEXT ),
 * - eglWaitClient
 * - eglWaitNative
 */
static void
bindAPI(EGLenum api) {
    if (eglBindAPI(api) != EGL_TRUE) {
        std::cerr << "error: eglBindAPI failed\n";
        exit(1);
    }
}


class EglVisual : public Visual
{
public:
    EGLConfig config;

    EglVisual(Profile prof) :
        Visual(prof),
        config(0) { }

    ~EglVisual() {
    }
};

static EGLSyncKHR
create_fence(int fd) {
    EGLint attrib_list[] = {
        EGL_SYNC_NATIVE_FENCE_FD_ANDROID,
        fd,
        EGL_NONE,
    };
    EGLSyncKHR fence = eglCreateSyncKHR(eglDisplay,
                                        EGL_SYNC_NATIVE_FENCE_ANDROID, attrib_list);
    assert(fence);
    return fence;
}

static int
add_crtc_property(drmModeAtomicReq *req, uint32_t obj_id,
                  const char *name, uint64_t value) {
    struct crtc *obj = drm->crtc;
    unsigned int i;
    int prop_id = -1;

    for (i = 0; i < obj->props->count_props; i++) {
        if (strcmp(obj->props_info[i]->name, name) == 0) {
            prop_id = obj->props_info[i]->prop_id;
            break;
        }
    }

    if (prop_id < 0) {
        printf("no crtc property: %s\n", name);
        return -EINVAL;
    }

    return drmModeAtomicAddProperty(req, obj_id, prop_id, value);
}

static int
add_plane_property(drmModeAtomicReq *req, uint32_t obj_id,
                   const char *name, uint64_t value) {
    struct plane *obj = drm->plane;
    unsigned int i;
    int prop_id = -1;

    for (i = 0; i < obj->props->count_props; i++) {
        if (strcmp(obj->props_info[i]->name, name) == 0) {
            prop_id = obj->props_info[i]->prop_id;
            break;
        }
    }


    if (prop_id < 0) {
        printf("no plane property: %s\n", name);
        return -EINVAL;
    }

    return drmModeAtomicAddProperty(req, obj_id, prop_id, value);
}

static int
add_connector_property(drmModeAtomicReq *req, uint32_t obj_id,
                       const char *name, uint64_t value) {
    struct connector *obj = drm->connector;
    unsigned int i;
    int prop_id = 0;

    for (i = 0; i < obj->props->count_props; i++) {
        if (strcmp(obj->props_info[i]->name, name) == 0) {
            prop_id = obj->props_info[i]->prop_id;
            break;
        }
    }

    if (prop_id < 0) {
        printf("no connector property: %s\n", name);
        return -EINVAL;
    }

    return drmModeAtomicAddProperty(req, obj_id, prop_id, value);
}

#define VOID2U64(x) ((uint64_t)(unsigned long)(x))

static int
drm_atomic_commit(uint32_t fb_id, uint32_t flags) {
    drmModeAtomicReq *req;
    uint32_t plane_id = drm->plane->plane->plane_id;
    uint32_t blob_id;
    int ret;

    req = drmModeAtomicAlloc();

    if (flags & DRM_MODE_ATOMIC_ALLOW_MODESET) {
        if (add_connector_property(req, drm->connector_id, "CRTC_ID",
                                   drm->crtc_id) < 0)
            return -1;

        if (drmModeCreatePropertyBlob(drm->fd, drm->mode, sizeof(*drm->mode),
                                      &blob_id) != 0)
            return -1;

        if (add_crtc_property(req, drm->crtc_id, "MODE_ID", blob_id) < 0)
            return -1;

        if (add_crtc_property(req, drm->crtc_id, "ACTIVE", 1) < 0)
            return -1;
    }

    add_plane_property(req, plane_id, "FB_ID", fb_id);
    add_plane_property(req, plane_id, "CRTC_ID", drm->crtc_id);
    add_plane_property(req, plane_id, "SRC_X", 0);
    add_plane_property(req, plane_id, "SRC_Y", 0);
    add_plane_property(req, plane_id, "SRC_W", drm->mode->hdisplay << 16);
    add_plane_property(req, plane_id, "SRC_H", drm->mode->vdisplay << 16);
    add_plane_property(req, plane_id, "CRTC_X", 0);
    add_plane_property(req, plane_id, "CRTC_Y", 0);
    add_plane_property(req, plane_id, "CRTC_W", drm->mode->hdisplay);
    add_plane_property(req, plane_id, "CRTC_H", drm->mode->vdisplay);

    if (drm->kms_in_fence_fd != -1) {
        add_crtc_property(req, drm->crtc_id, "OUT_FENCE_PTR",
                          VOID2U64(&drm->kms_out_fence_fd));
        add_plane_property(req, plane_id, "IN_FENCE_FD", drm->kms_in_fence_fd);
    }

    ret = drmModeAtomicCommit(drm->fd, req, flags, NULL);
    if (ret)
        goto out;

    if (drm->kms_in_fence_fd != -1) {
        close(drm->kms_in_fence_fd);
        ((struct drm *)drm)->kms_in_fence_fd = -1;
    }

out:
    drmModeAtomicFree(req);

    return ret;
}

class EglDrawable : public Drawable
{
public:
    EGLSurface surface;
    EGLenum api;

    struct gbm_bo *bo;
    struct drm_fb *fb;
    uint32_t frame_count = 0;

    int64_t start_time, report_time, cur_time;
    int ret;

    uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK;

    PFNEGLDUPNATIVEFENCEFDANDROIDPROC eglDupNativeFenceFDANDROID;

    EglDrawable(const Visual *vis, int w, int h,
                const glws::pbuffer_info *pbInfo) :
        Drawable(vis, w, h, pbInfo),
        api(EGL_OPENGL_ES_API) {
        eglWaitNative(EGL_CORE_NATIVE_ENGINE);

        eglDupNativeFenceFDANDROID = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");

        EGLConfig config = static_cast<const EglVisual *>(visual)->config;
        surface = eglCreateWindowSurface(eglDisplay, config, (EGLNativeWindowType)gbm->surface, NULL);
    }

    ~EglDrawable() {
        eglDestroySurface(eglDisplay, surface);
        eglWaitClient();
        eglWaitNative(EGL_CORE_NATIVE_ENGINE);
    }

    void
    recreate(void) {
        EGLContext currentContext = eglGetCurrentContext();
        EGLSurface currentDrawSurface = eglGetCurrentSurface(EGL_DRAW);
        EGLSurface currentReadSurface = eglGetCurrentSurface(EGL_READ);
        bool rebindDrawSurface = currentDrawSurface == surface;
        bool rebindReadSurface = currentReadSurface == surface;

        if (rebindDrawSurface || rebindReadSurface) {
            eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }

        // XXX: Defer destruction to prevent getting the same surface as before, which seems to cause Mesa to crash
        EGLSurface oldSurface = surface;

        EGLConfig config = static_cast<const EglVisual *>(visual)->config;
        surface = eglCreateWindowSurface(eglDisplay, config, (EGLNativeWindowType)gbm->surface, NULL);
        if (surface == EGL_NO_SURFACE) {
            // XXX: But don't defer destruction if eglCreateWindowSurface fails, which is the case of SwiftShader
            eglDestroySurface(eglDisplay, oldSurface);
            oldSurface = EGL_NO_SURFACE;
            surface = eglCreateWindowSurface(eglDisplay, config, (EGLNativeWindowType)gbm->surface, NULL);
        }
        assert(surface != EGL_NO_SURFACE);

        if (rebindDrawSurface || rebindReadSurface) {
            eglMakeCurrent(eglDisplay, surface, surface, currentContext);
        }

        if (oldSurface != EGL_NO_SURFACE) {
            eglDestroySurface(eglDisplay, oldSurface);
        }
    }

    void
    resize(int w, int h) override {
        if (w == width && h == height) {
            return;
        }

        eglWaitClient();

        Drawable::resize(w, h);

        eglWaitNative(EGL_CORE_NATIVE_ENGINE);

        /*
         * Some implementations won't update the backbuffer unless we recreate
         * the EGL surface.
         */

        int eglWidth;
        int eglHeight;

        eglQuerySurface(eglDisplay, surface, EGL_WIDTH, &eglWidth);
        eglQuerySurface(eglDisplay, surface, EGL_HEIGHT, &eglHeight);


        /*
        if (eglWidth != width || eglHeight != height) {
            recreate();

            eglQuerySurface(eglDisplay, surface, EGL_WIDTH, &eglWidth);
            eglQuerySurface(eglDisplay, surface, EGL_HEIGHT, &eglHeight);
        }
        */

        printf("EGL surface size %dx%d\n", eglWidth, eglHeight);
        printf("Trace dimensions %dx%d\n", width, height);

        // assert(eglWidth == width);
        // assert(eglHeight == height);
    }

    void show(void) override {
        if (visible) {
            return;
        }

        if (!checkExtension("EGL_ANDROID_native_fence_sync", eglExtensions)) {
            printf("EGL_ANDROID_native_fence_sync not available.\n");
            return;
        }

        if (!eglDupNativeFenceFDANDROID ||
            !eglCreateSyncKHR ||
            !eglDestroySyncKHR ||
            !eglWaitSyncKHR ||
            !eglClientWaitSyncKHR) {
            printf("Extensions not available.\n");
            return;
        }

        /* Allow a modeset change for the first commit only. */
        flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;

        start_time = report_time = get_time_ns();

        eglWaitClient();

        eglWaitNative(EGL_CORE_NATIVE_ENGINE);

        Drawable::show();
    }

    void swapBuffers(void) override {
        bindAPI(api);

        struct gbm_bo *next_bo;
        EGLSyncKHR gpu_fence = NULL; /* out-fence from gpu, in-fence to kms */
        EGLSyncKHR kms_fence = NULL; /* in-fence to gpu, out-fence from kms */

        if (drm->kms_out_fence_fd != -1) {
            kms_fence = create_fence(drm->kms_out_fence_fd);
            assert(kms_fence);

            /* driver now has ownership of the fence fd: */
            ((struct drm *)drm)->kms_out_fence_fd = -1;

            /* wait "on the gpu" (ie. this won't necessarily block, but
             * will block the rendering until fence is signaled), until
             * the previous pageflip completes so we don't render into
             * the buffer that is still on screen.
             */
            eglWaitSyncKHR(eglDisplay, kms_fence, 0);
        }


        /* Start fps measuring on second frame, to remove the time spent
         * compiling shader, etc, from the fps:
         */
        if (frame_count == 1) {
            start_time = report_time = get_time_ns();
        }

        if (!gbm->surface) {
            // glBindFramebuffer(GL_FRAMEBUFFER, egl->fbs[frame % NUM_BUFFERS].fb);
        }

        frame_count++;
        // TODO: kmscube drew here, probably move to fence stuff to the bottom.
        // egl->draw(i++);

        /* insert fence to be singled in cmdstream.. this fence will be
         * signaled when gpu rendering done
         */
        gpu_fence = create_fence(EGL_NO_NATIVE_FENCE_FD_ANDROID);
        assert(gpu_fence);

        if (gbm->surface) {
            eglSwapBuffers(eglDisplay, surface);
        }

        /* after swapbuffers, gpu_fence should be flushed, so safe
         * to get fd:
         */

        ((struct drm *)drm)->kms_in_fence_fd = eglDupNativeFenceFDANDROID(eglDisplay, gpu_fence);
        eglDestroySyncKHR(eglDisplay, gpu_fence);
        assert(drm->kms_in_fence_fd != -1);

        if (gbm->surface) {
            next_bo = gbm_surface_lock_front_buffer(gbm->surface);
        } else {
            next_bo = gbm->bos[frame_count % NUM_BUFFERS];
        }
        if (!next_bo) {
            printf("Failed to lock frontbuffer\n");
            return;
        }
        fb = drm_fb_get_from_bo(next_bo);
        if (!fb) {
            printf("Failed to get a new framebuffer BO\n");
            return;
        }

        if (kms_fence) {
            EGLint status;

            /* Wait on the CPU side for the _previous_ commit to
             * complete before we post the flip through KMS, as
             * atomic will reject the commit if we post a new one
             * whilst the previous one is still pending.
             */
            do {
                status = eglClientWaitSyncKHR(eglDisplay,
                                              kms_fence,
                                              0,
                                              EGL_FOREVER_KHR);
            } while (status != EGL_CONDITION_SATISFIED_KHR);

            eglDestroySyncKHR(eglDisplay, kms_fence);
        }

        cur_time = get_time_ns();
        if (cur_time > (report_time + 2 * NSEC_PER_SEC)) {
            double elapsed_time = cur_time - start_time;
            double secs = elapsed_time / (double)NSEC_PER_SEC;
            unsigned frames = frame_count - 1; /* first frame ignored */
            printf("Rendered %d frames in %f sec (%f fps)\n",
                   frames, secs, (double)frames / secs);
            report_time = cur_time;
        }

        /* Check for user input: */
        struct pollfd fdset[] = { {
            .fd = STDIN_FILENO,
            .events = POLLIN,
        } };
        ret = poll(fdset, ARRAY_SIZE(fdset), 0);
        if (ret > 0) {
            printf("user interrupted!\n");
            return;
        }

        /*
         * Here you could also update drm plane layers if you want
         * hw composition
         */
        ret = drm_atomic_commit(fb->fb_id, flags);
        if (ret) {
            printf("failed to commit: %s\n", strerror(errno));
            return;
        }

        /* release last buffer to render on again: */
        if (bo && gbm->surface)
            gbm_surface_release_buffer(gbm->surface, bo);
        bo = next_bo;

        /* Allow a modeset change for the first commit only. */
        flags &= ~(DRM_MODE_ATOMIC_ALLOW_MODESET);
    }
};


class EglContext : public Context
{
public:
    EGLContext context;

    EglContext(const Visual *vis, EGLContext ctx) :
        Context(vis),
        context(ctx) { }

    ~EglContext() {
        eglDestroyContext(eglDisplay, context);
    }
};

/**
 * Load the symbols from the specified shared object into global namespace, so
 * that they can be later found by dlsym(RTLD_NEXT, ...);
 */
static void
load(const char *filename) {
    if (!dlopen(filename, RTLD_GLOBAL | RTLD_LAZY)) {
        std::cerr << "error: unable to open " << filename << "\n";
        exit(1);
    }
}

void
init(void) {
    load("libEGL.so.1");

    const char *device = NULL;
    char mode_str[DRM_DISPLAY_MODE_LEN] = "";
    unsigned int vrefresh = 0;
    unsigned int count = ~0;
    drm = init_drm_atomic(device, mode_str, vrefresh, count);

    if (!drm) {
        printf("failed to initialize DRM\n");
        return;
    }

    uint32_t format = DRM_FORMAT_XRGB8888;
    uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
    bool surfaceless = false;

    gbm = init_gbm(drm->fd, drm->mode->hdisplay, drm->mode->vdisplay,
                   format, modifier, surfaceless);
    if (!gbm) {
        printf("failed to initialize GBM\n");
        return;
    }

    eglExtensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);

    if (eglGetPlatformDisplayEXT) {
        eglDisplay = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR,
                                              gbm->dev, NULL);
    } else {
        eglDisplay = eglGetDisplay((void *)gbm->dev);
    }

    if (eglDisplay == EGL_NO_DISPLAY) {
        std::cerr << "error: unable to get EGL display\n";
        exit(1);
    }

    EGLint major, minor;
    if (!eglInitialize(eglDisplay, &major, &minor)) {
        std::cerr << "error: unable to initialize EGL display\n";
        exit(1);
    }

    eglExtensions = eglQueryString(eglDisplay, EGL_EXTENSIONS);
    has_EGL_KHR_create_context = checkExtension("EGL_KHR_create_context", eglExtensions);
}

void
cleanup(void) {
    if (eglDisplay != EGL_NO_DISPLAY) {
        eglTerminate(eglDisplay);
    }
}

Visual *
createVisual(bool doubleBuffer, unsigned samples, Profile profile) {
    EGLint api_bits;
    if (profile.api == glfeatures::API_GL) {
        api_bits = EGL_OPENGL_BIT;
        if (profile.core && !has_EGL_KHR_create_context) {
            return NULL;
        }
    } else if (profile.api == glfeatures::API_GLES) {
        switch (profile.major) {
        case 1:
            api_bits = EGL_OPENGL_ES_BIT;
            break;
        case 3:
            if (has_EGL_KHR_create_context) {
                api_bits = EGL_OPENGL_ES3_BIT;
                break;
            }
            /* fall-through */
        case 2:
            api_bits = EGL_OPENGL_ES2_BIT;
            break;
        default:
            return NULL;
        }
    } else {
        assert(0);
        return NULL;
    }

    Attributes<EGLint> attribs;
    attribs.add(EGL_SURFACE_TYPE, EGL_WINDOW_BIT);
    attribs.add(EGL_RED_SIZE, 8);
    attribs.add(EGL_GREEN_SIZE, 8);
    attribs.add(EGL_BLUE_SIZE, 8);
    attribs.add(EGL_ALPHA_SIZE, 8);
    attribs.add(EGL_DEPTH_SIZE, 24);
    attribs.add(EGL_STENCIL_SIZE, 8);
    attribs.add(EGL_RENDERABLE_TYPE, api_bits);
    attribs.end(EGL_NONE);

    EGLint num_configs = 0;
    if (!eglGetConfigs(eglDisplay, NULL, 0, &num_configs) ||
        num_configs <= 0) {
        return NULL;
    }

    std::vector<EGLConfig> configs(num_configs);
    if (!eglChooseConfig(eglDisplay, attribs, &configs[0], num_configs, &num_configs) ||
        num_configs <= 0) {
        return NULL;
    }

    // We can't tell what other APIs the trace will use afterwards, therefore
    // try to pick a config which supports the widest set of APIs.
    int bestScore = -1;
    EGLConfig config = configs[0];
    for (EGLint i = 0; i < num_configs; ++i) {
        EGLint renderable_type = EGL_NONE;
        eglGetConfigAttrib(eglDisplay, configs[i], EGL_RENDERABLE_TYPE, &renderable_type);
        int score = 0;
        assert(renderable_type & api_bits);
        renderable_type &= ~api_bits;
        if (renderable_type & EGL_OPENGL_ES2_BIT) {
            score += 1 << 4;
        }
        if (renderable_type & EGL_OPENGL_ES3_BIT) {
            score += 1 << 3;
        }
        if (renderable_type & EGL_OPENGL_ES_BIT) {
            score += 1 << 2;
        }
        if (renderable_type & EGL_OPENGL_BIT) {
            score += 1 << 1;
        }
        if (score > bestScore) {
            config = configs[i];
            bestScore = score;
        }
    }
    assert(bestScore >= 0);

    EGLint visual_id;
    if (!eglGetConfigAttrib(eglDisplay, config, EGL_NATIVE_VISUAL_ID, &visual_id)) {
        assert(0);
        return NULL;
    }

    EglVisual *visual = new EglVisual(profile);
    visual->config = config;

    return visual;
}

bool
processEvents(void) {
    return true;
}


Drawable *
createDrawable(const Visual *visual, int width, int height,
               const glws::pbuffer_info *pbInfo) {
    return new EglDrawable(visual, width, height, pbInfo);
}


Context *
createContext(const Visual *_visual, Context *shareContext, bool debug) {
    Profile profile = _visual->profile;
    const EglVisual *visual = static_cast<const EglVisual *>(_visual);
    EGLContext share_context = EGL_NO_CONTEXT;
    EGLContext context;
    Attributes<EGLint> attribs;

    if (shareContext) {
        share_context = static_cast<EglContext *>(shareContext)->context;
    }

    int contextFlags = 0;
    if (profile.api == glfeatures::API_GL) {
        load("libGL.so.1");

        if (has_EGL_KHR_create_context) {
            attribs.add(EGL_CONTEXT_MAJOR_VERSION_KHR, profile.major);
            attribs.add(EGL_CONTEXT_MINOR_VERSION_KHR, profile.minor);
            int profileMask = profile.core ? EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR : EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR;
            attribs.add(EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, profileMask);
            if (profile.forwardCompatible) {
                contextFlags |= EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR;
            }
        } else if (profile.versionGreaterOrEqual(3, 2)) {
            std::cerr << "error: EGL_KHR_create_context not supported\n";
            return NULL;
        }
    } else if (profile.api == glfeatures::API_GLES) {
        if (profile.major >= 2) {
            load("libGLESv2.so.2");
        } else {
            load("libGLESv1_CM.so.1");
        }

        if (has_EGL_KHR_create_context) {
            attribs.add(EGL_CONTEXT_MAJOR_VERSION_KHR, profile.major);
            attribs.add(EGL_CONTEXT_MINOR_VERSION_KHR, profile.minor);
        } else {
            attribs.add(EGL_CONTEXT_CLIENT_VERSION, profile.major);
        }
    } else {
        assert(0);
        return NULL;
    }

    if (debug) {
        contextFlags |= EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR;
    }
    if (contextFlags && has_EGL_KHR_create_context) {
        attribs.add(EGL_CONTEXT_FLAGS_KHR, contextFlags);
    }
    attribs.end(EGL_NONE);

    EGLenum api = translateAPI(profile);
    bindAPI(api);

    context = eglCreateContext(eglDisplay, visual->config, share_context, attribs);
    if (!context) {
        if (debug) {
            // XXX: Mesa has problems with EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR
            // with OpenGL ES contexts, so retry without it
            return createContext(_visual, shareContext, false);
        }
        return NULL;
    }

    return new EglContext(visual, context);
}

bool
makeCurrentInternal(Drawable *drawable, Drawable *readable, Context *context) {
    if (!drawable || !context) {
        return eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    } else {
        EglDrawable *eglDrawable = static_cast<EglDrawable *>(drawable);
        EglDrawable *eglReadable = static_cast<EglDrawable *>(readable);
        EglContext *eglContext = static_cast<EglContext *>(context);
        EGLBoolean ok;

        EGLenum api = translateAPI(eglContext->profile);
        bindAPI(api);

        ok = eglMakeCurrent(eglDisplay, eglDrawable->surface,
                            eglReadable->surface, eglContext->context);

        if (ok) {
            eglDrawable->api = api;
            eglReadable->api = api;
        }

        return ok;
    }
}

bool
bindTexImage(Drawable *pBuffer, int iBuffer) {
    std::cerr << "error: EGL/drm::wglBindTexImageARB not implemented.\n";
    assert(pBuffer->pbuffer);
    return true;
}

bool
releaseTexImage(Drawable *pBuffer, int iBuffer) {
    std::cerr << "error: EGL/drm::wglReleaseTexImageARB not implemented.\n";
    assert(pBuffer->pbuffer);
    return true;
}

bool
setPbufferAttrib(Drawable *pBuffer, const int *attribList) {
    // nothing to do here.
    assert(pBuffer->pbuffer);
    return true;
}


} /* namespace glws */
