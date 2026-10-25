/* BootPanel — boot color layout constants (spec, do not change lightly). */

#ifndef BOOTPANEL_H
#define BOOTPANEL_H

#include <stdbool.h>
#include <stdint.h>

/* Panel background, the color also baked into the art plates. */
#define BOOTPANEL_BG_R 191
#define BOOTPANEL_BG_G 191
#define BOOTPANEL_BG_B 191

/* Spinner animation: 12 frames, cycling 00..11. */
#define BOOTPANEL_SPINNER_FRAMES 12

/* Spinner center: x = display width center,
 *                 y = display height center + 185 px. */
#define BOOTPANEL_SPINNER_OFFSET_Y 185

/* Logo, spinner frames — raw pixels, from assets/ via tools/genassets.py. */
const uint8_t *bootpanel_logo(int *w, int *h);        /* RGBA, 4 bytes/px */
const uint8_t *bootpanel_spinner(int frame, int *w, int *h); /* gray, 1 byte/px */

/* An image to place on the panel: `px` is w*h bytes for the gray spinner
 * frames, w*h*4 bytes of RGBA for the logo — exactly as generated. */
typedef struct {
    int w, h;
    const uint8_t *px;
} sprite_t;

/* A drawable screen surface as reported by the active backend. 3/4-byte
 * pixels are little-endian; a format is described by component bit offsets,
 * like the fbdev API. */
typedef struct {
    uint8_t *map;
    int w, h;
    int stride;
    int bpp;                    /* 16 | 24 | 32 */
    int r_off, g_off, b_off;    /* component bit offsets */
    int r_len, g_len, b_len;    /* component bit widths */
} screen_t;

/* Dissuade kernel text output under our feet (KD_GRAPHICS on the console)
 * and undo it again. Failures are silent: no tty at boot is fine. */
void textconsole_inhibit(void);
void textconsole_restore(void);

/* Draw a whole animation frame: background, logo, spinner `frame`. Clips. */
void bootpanel_draw(const screen_t *s, int frame);

/* Read a pixel (for --screenshot). */
void screen_get_px(const screen_t *s, int x, int y,
                   uint8_t *r, uint8_t *g, uint8_t *b);

/* A display backend. acquire() returns the surface to draw into; present()
 * puts it in front (page flip for kms, memcpy-less for fbdev). */
typedef struct backend backend_t;
struct backend {
    const char *name;
    void *ctx;
    int (*open)(backend_t *self, const char *device);
    const screen_t *(*acquire)(backend_t *self);
    void (*present)(backend_t *self);
    void (*close)(backend_t *self);
};

/* Legacy framebuffer, /dev/fb0. Always compiled. */
backend_t *backend_fbdev(void);

/* KMS/DRM dumb buffers + page flip. NULL when built without libdrm. */
backend_t *backend_kms(void);

#endif
