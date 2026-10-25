/*
 * BootPanel — legacy framebuffer backend (/dev/fb0).
 * No kernel modesetting: mmap what is already running and draw into it.
 * MIT — see LICENSE.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/fb.h>

#include "bootpanel.h"

typedef struct {
    int fd;
    screen_t scr;
    uint8_t *off;   /* offscreen compose buffer, then one memcpy to vis */
    uint8_t *vis;
    size_t len;
} fbdev_t;

static int fbdev_open(backend_t *self, const char *device)
{
    fbdev_t *b = self->ctx;

    b->fd = open(device && *device ? device : "/dev/fb0",
                 O_RDWR | O_CLOEXEC);
    if (b->fd < 0)
        return -1;

    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(b->fd, FBIOGET_VSCREENINFO, &var) != 0 ||
        ioctl(b->fd, FBIOGET_FSCREENINFO, &fix) != 0 ||
        var.xres <= 0 || var.yres <= 0) {
        fprintf(stderr, "bootpanel: fbdev: bad framebuffer info: %s\n",
                strerror(errno));
        close(b->fd);
        b->fd = -1;
        return -1;
    }

    int bpp = (int)var.bits_per_pixel;
    if (bpp != 32 && bpp != 24 && bpp != 16) {
        fprintf(stderr, "bootpanel: fbdev: unsupported %d bpp\n", bpp);
        close(b->fd);
        b->fd = -1;
        return -1;
    }

    size_t len = (size_t)fix.smem_len > 0
        ? (size_t)fix.smem_len
        : (size_t)fix.line_length * var.yres_virtual;
    uint8_t *vis = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED,
                        b->fd, 0);
    if (vis == MAP_FAILED) {
        fprintf(stderr, "bootpanel: fbdev: mmap failed: %s\n",
                strerror(errno));
        close(b->fd);
        b->fd = -1;
        return -1;
    }

    memset(&b->scr, 0, sizeof(b->scr));
    b->scr.map = vis; /* replaced by the offscreen buffer below */
    b->scr.w = (int)var.xres;
    b->scr.h = (int)var.yres;
    b->scr.stride = (int)(fix.line_length ? fix.line_length : var.xres * bpp / 8);
    b->scr.bpp = bpp;
    b->scr.r_off = (int)var.red.offset;
    b->scr.g_off = (int)var.green.offset;
    b->scr.b_off = (int)var.blue.offset;
    b->scr.r_len = (int)var.red.length;
    b->scr.g_len = (int)var.green.length;
    b->scr.b_len = (int)var.blue.length;
    if (bpp != 16) {
        /* make 32/24 handling safe: byte-aligned components assumed */
        b->scr.r_off &= ~7;
        b->scr.g_off &= ~7;
        b->scr.b_off &= ~7;
    }

    /* Compose each frame offscreen and put it up with one memcpy: writing
     * the panel piece by piece into the visible buffer lets the scanout
     * catch half-drawn states (visible flashing, reported in testing). */
    b->vis = vis;
    b->len = (size_t)b->scr.stride * (size_t)b->scr.h;
    b->off = malloc(b->len);
    if (!b->off) {
        fprintf(stderr, "bootpanel: fbdev: out of memory\n");
        munmap(vis, len);
        close(b->fd);
        b->fd = -1;
        return -1;
    }
    b->scr.map = b->off;
    return 0;
}

static const screen_t *fbdev_acquire(backend_t *self)
{
    fbdev_t *b = self->ctx;
    return &b->scr;
}

static void fbdev_present(backend_t *self)
{
    fbdev_t *b = self->ctx;
    if (b->off && b->vis)
        memcpy(b->vis, b->off, b->len);
}

static void fbdev_close(backend_t *self)
{
    fbdev_t *b = self->ctx;
    if (b->vis && b->vis != MAP_FAILED) {
        /* Fine to leave the last frame up: the boot panel hands the screen
         * over to the desktop, which repaints everything anyway. */
        munmap(b->vis, b->len);
        b->vis = NULL;
    }
    free(b->off);
    b->off = NULL;
    b->scr.map = NULL;
    if (b->fd >= 0) {
        close(b->fd);
        b->fd = -1;
    }
}

backend_t *backend_fbdev(void)
{
    static backend_t self;
    static fbdev_t ctx;

    self.name = "fbdev";
    self.ctx = &ctx;
    self.open = fbdev_open;
    self.acquire = fbdev_acquire;
    self.present = fbdev_present;
    self.close = fbdev_close;
    return &self;
}
