/*
 * BootPanel — KMS/DRM backend: open the first card with a connected
 * display, modeset DUMB buffers and page flip between two of them.
 * This is the preferred backend; the legacy fbdev one is the fallback.
 * MIT — see LICENSE.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "bootpanel.h"

#ifndef HAVE_KMS
/* built without libdrm: no KMS backend */
backend_t *backend_kms(void)
{
    return NULL;
}
#else
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

typedef struct {
    uint32_t fb;
    uint8_t *map;
    int stride;
    size_t size;
    uint32_t handle;
} drm_fb_t;

typedef struct {
    int fd;
    uint32_t conn_id, crtc_id, enc_id;
    drmModeModeInfo mode;
    drm_fb_t fb[2];
    int cur;
    drmModeResPtr res;
    drmModeConnectorPtr conn;
    drmModeCrtcPtr saved;
    int flipped;
} drm_t;

static int dumb_open(drm_t *d, drm_fb_t *f, int index)
{
    struct drm_mode_create_dumb creq;
    struct drm_mode_map_dumb mreq;

    memset(&creq, 0, sizeof(creq));
    creq.width = (uint32_t)d->mode.hdisplay;
    creq.height = (uint32_t)d->mode.vdisplay;
    creq.bpp = 32;
    if (drmIoctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) != 0) {
        fprintf(stderr, "bootpanel: kms: create dumb buffer: %s\n",
                strerror(errno));
        return -1;
    }
    f->handle = creq.handle;
    f->stride = (int)creq.pitch;
    f->size = creq.size;

    if (drmModeAddFB(d->fd, creq.width, creq.height, 24, 32, creq.pitch,
                     creq.handle, &f->fb) != 0) {
        fprintf(stderr, "bootpanel: kms: add fb: %s\n", strerror(errno));
        return -1;
    }

    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = creq.handle;
    if (drmIoctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) != 0) {
        fprintf(stderr, "bootpanel: kms: map dumb: %s\n", strerror(errno));
        return -1;
    }
    f->map = mmap(NULL, f->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  d->fd, (off_t)mreq.offset);
    if (f->map == MAP_FAILED) {
        f->map = NULL;
        fprintf(stderr, "bootpanel: kms: mmap: %s\n", strerror(errno));
        return -1;
    }
    (void)index;
    return 0;
}

static int find_display(drm_t *d)
{
    d->res = drmModeGetResources(d->fd);
    if (!d->res)
        return -1;

    for (int i = 0; i < d->res->count_connectors; i++) {
        drmModeConnectorPtr c = drmModeGetConnector(d->fd, d->res->connectors[i]);
        if (!c)
            continue;
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
            d->conn = c;
            d->conn_id = c->connector_id;
            break;
        }
        drmModeFreeConnector(c);
    }
    if (!d->conn)
        return -1;

    d->enc_id = d->conn->encoder_id;
    drmModeEncoderPtr e = drmModeGetEncoder(d->fd, d->enc_id);
    if (e) {
        d->crtc_id = e->crtc_id;
        drmModeFreeEncoder(e);
    }
    if (!d->crtc_id) {
        /* unbound connector: try a crtc its encoders can possibly drive */
        for (int e = 0; e < d->conn->count_encoders && !d->crtc_id; e++) {
            drmModeEncoderPtr en =
                drmModeGetEncoder(d->fd, d->conn->encoders[e]);
            if (!en)
                continue;
            for (int i = 0; i < d->res->count_crtcs; i++) {
                if (!(en->possible_crtcs & (1u << i)))
                    continue;
                uint32_t cid = d->res->crtcs[i];
                drmModeCrtcPtr cu = drmModeGetCrtc(d->fd, cid);
                int is_free = cu && !cu->buffer_id && !cu->mode_valid;
                if (cu)
                    drmModeFreeCrtc(cu);
                if (is_free) {
                    d->crtc_id = cid;
                    break;
                }
            }
            drmModeFreeEncoder(en);
        }
    }
    if (!d->crtc_id)
        return -1;

    /* preferred mode, else the largest */
    d->mode = d->conn->modes[0];
    for (int i = 0; i < d->conn->count_modes; i++)
        if (d->conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            d->mode = d->conn->modes[i];
            break;
        }
    return 0;
}

static int kms_open(backend_t *self, const char *device)
{
    drm_t *d = self->ctx;
    static char buf[64];

    if (device && *device) {
        d->fd = open(device, O_RDWR | O_CLOEXEC);
    } else {
        for (int i = 0; i < 16 && d->fd < 0; i++) {
            snprintf(buf, sizeof(buf), "/dev/dri/card%d", i);
            d->fd = open(buf, O_RDWR | O_CLOEXEC);
        }
    }
    if (d->fd < 0)
        return -1;

    if (find_display(d) != 0)
        goto err;

    for (int i = 0; i < 2; i++)
        if (dumb_open(d, &d->fb[i], i) != 0)
            goto err;

    if (drmSetMaster(d->fd) != 0)
        fprintf(stderr, "bootpanel: kms: not master (%s); "
                "continuing anyway\n", strerror(errno));

    d->saved = drmModeGetCrtc(d->fd, d->crtc_id);
    if (drmModeSetCrtc(d->fd, d->crtc_id, d->fb[0].fb, 0, 0, &d->conn_id,
                       1, &d->mode) != 0) {
        fprintf(stderr, "bootpanel: kms: modeset (%s)\n", strerror(errno));
        if (d->saved) {
            drmModeFreeCrtc(d->saved);
            d->saved = NULL;
        }
        goto err;
    }

    return 0;

err:
    fprintf(stderr, "bootpanel: kms: no usable display on that device\n");
    return -1;
}

static const screen_t *kms_acquire(backend_t *self)
{
    drm_t *d = self->ctx;
    static screen_t scr;

    drm_fb_t *f = &d->fb[d->cur ^ 1];
    memset(&scr, 0, sizeof(scr));
    scr.map = f->map;
    scr.w = (int)d->mode.hdisplay;
    scr.h = (int)d->mode.vdisplay;
    scr.stride = f->stride;
    scr.bpp = 32;
    scr.r_off = 16;
    scr.g_off = 8;
    scr.b_off = 0;
    scr.r_len = 8;
    scr.g_len = 8;
    scr.b_len = 8;
    return &scr;
}

static void kms_present(backend_t *self)
{
    drm_t *d = self->ctx;

    d->cur ^= 1;
    if (drmModePageFlip(d->fd, d->crtc_id, d->fb[d->cur].fb,
                        0, NULL) == 0)
        return;
    /* fallback: straight modeset (slower but works anywhere) */
    drmModeSetCrtc(d->fd, d->crtc_id, d->fb[d->cur].fb, 0, 0, &d->conn_id,
                   1, &d->mode);
}

static void kms_close(backend_t *self)
{
    drm_t *d = self->ctx;

    if (d->saved) {
        drmModeSetCrtc(d->fd, d->saved->crtc_id, d->saved->buffer_id,
                       d->saved->x, d->saved->y, NULL, 0, &d->saved->mode);
        drmModeFreeCrtc(d->saved);
        d->saved = NULL;
    }
    for (int i = 0; i < 2; i++) {
        drm_fb_t *f = &d->fb[i];
        if (f->map)
            munmap(f->map, f->size);
        if (f->fb)
            drmModeRmFB(d->fd, f->fb);
        if (f->handle) {
            struct drm_mode_destroy_dumb da;
            da.handle = f->handle;
            drmIoctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &da);
        }
    }
    if (d->conn)
        drmModeFreeConnector(d->conn);
    if (d->res)
        drmModeFreeResources(d->res);
    if (d->fd >= 0) {
        drmModeSetCrtc(d->fd, 0, 0, 0, 0, NULL, 0, NULL);
        drmDropMaster(d->fd);
        close(d->fd);
        d->fd = -1;
    }
}

backend_t *backend_kms(void)
{
    static backend_t self;
    static drm_t ctx;

    self.name = "kms";
    self.ctx = &ctx;
    self.open = kms_open;
    self.acquire = kms_acquire;
    self.present = kms_present;
    self.close = kms_close;
    return &self;
}
#endif
