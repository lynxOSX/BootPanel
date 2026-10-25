/*
 * BootPanel — the lynxOS boot panel (a plymouth-alike).
 *
 * Layout (spec):
 *   - background: hardcoded #BFBFBF over the whole display (the color the
 *     art plates are baked at, so sprites land seamlessly on it);
 *   - logo: centered in the display;
 *   - spinner: x center = display width center,
 *              y center = display height center + 185 px.
 *
 * MIT — see LICENSE.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/kd.h>
#include <unistd.h>

#include "bootpanel.h"
#include "assets_embed.h"

const uint8_t *bootpanel_logo(int *w, int *h)
{
    if (w)
        *w = BOOTPANEL_LOGO_W;
    if (h)
        *h = BOOTPANEL_LOGO_H;
    return bootpanel_logo_px;
}

const uint8_t *bootpanel_spinner(int frame, int *w, int *h)
{
    static const uint8_t *const frames[BOOTPANEL_SPINNER_FRAMES] = {
        bootpanel_sp00, bootpanel_sp01, bootpanel_sp02, bootpanel_sp03,
        bootpanel_sp04, bootpanel_sp05, bootpanel_sp06, bootpanel_sp07,
        bootpanel_sp08, bootpanel_sp09, bootpanel_sp10, bootpanel_sp11,
    };
    if (w)
        *w = BOOTPANEL_SPINNER_W;
    if (h)
        *h = BOOTPANEL_SPINNER_H;
    frame %= BOOTPANEL_SPINNER_FRAMES;
    if (frame < 0)
        frame += BOOTPANEL_SPINNER_FRAMES;
    return frames[frame];
}

/* --- text console cooperation --------------------------------------------- */

static int tty_inhibited;
static int tty_fd = -1;

void textconsole_inhibit(void)
{
    /* Hush the kernel console text/cursor while we own the display. */
    tty_fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if (tty_fd < 0)
        tty_fd = open("/dev/console", O_RDWR | O_CLOEXEC);
    if (tty_fd < 0)
        return;
    if (ioctl(tty_fd, KDSETMODE, KD_GRAPHICS) == 0)
        tty_inhibited = 1;
    else {
        close(tty_fd);
        tty_fd = -1;
    }
}

void textconsole_restore(void)
{
    if (tty_fd >= 0) {
        if (tty_inhibited) {
            ioctl(tty_fd, KDSETMODE, KD_TEXT);
            tty_inhibited = 0;
        }
        close(tty_fd);
        tty_fd = -1;
    }
}

/* --- pixel plumbing -------------------------------------------------------- */

static inline uint8_t blend(uint8_t dst, uint8_t src, uint8_t a)
{
    if (a == 255)
        return src;
    return (uint8_t)((dst * (255 - (int)a) + src * (int)a + 127) / 255);
}

/* 8-bit/component formats only; a component must sit byte-aligned. */
static void put_px(const screen_t *s, int x, int y,
                   uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (a == 0)
        return;
    uint8_t *px = s->map + (size_t)y * (size_t)s->stride;
    if (s->bpp == 32) {
        px[x * 4 + s->b_off / 8] = blend(px[x * 4 + s->b_off / 8], b, a);
        px[x * 4 + s->g_off / 8] = blend(px[x * 4 + s->g_off / 8], g, a);
        px[x * 4 + s->r_off / 8] = blend(px[x * 4 + s->r_off / 8], r, a);
    } else if (s->bpp == 24) {
        px[x * 3 + s->b_off / 8] = b;
        px[x * 3 + s->g_off / 8] = g;
        px[x * 3 + s->r_off / 8] = r;
    } else { /* 16: pack into the little-endian word, preserving spare bits */
        uint16_t old = (uint16_t)(px[x * 2 + 0] | (px[x * 2 + 1] << 8));
        uint16_t nv = 0;
        int shift[3] = { s->r_off, s->g_off, s->b_off };
        int len[3] = { s->r_len, s->g_len, s->b_len };
        int c48[3] = { r, g, b };
        uint16_t keep = 0xffff;
        for (int i = 0; i < 3; i++) {
            uint8_t c = c48[i];
            if (len[i] > 0 && len[i] < 8)
                c = (uint8_t)(c >> (8 - len[i]));
            nv |= (uint16_t)(c << shift[i]);
            keep &= (uint16_t)~(((len[i] > 0 &&
                                  len[i] < 8) ? ((1u << len[i]) - 1) :
                                  (len[i] == 0 ? 0u : 0xffu)) << shift[i]);
        }
        nv |= (uint16_t)(old & keep);
        px[x * 2 + 0] = (uint8_t)(nv & 0xff);
        px[x * 2 + 1] = (uint8_t)(nv >> 8);
    }
}

void screen_get_px(const screen_t *s, int x, int y,
                   uint8_t *r, uint8_t *g, uint8_t *b)
{
    const uint8_t *px = s->map + (size_t)y * (size_t)s->stride;
    if (s->bpp == 32) {
        *b = px[x * 4 + s->b_off / 8];
        *g = px[x * 4 + s->g_off / 8];
        *r = px[x * 4 + s->r_off / 8];
    } else if (s->bpp == 24) {
        *b = px[x * 3 + s->b_off / 8];
        *g = px[x * 3 + s->g_off / 8];
        *r = px[x * 3 + s->r_off / 8];
    } else if (s->bpp == 16) {
        uint16_t v = (uint16_t)(px[x * 2 + 0] | (px[x * 2 + 1] << 8));
        *r = (uint8_t)(((v >> s->r_off) & ((1u << s->r_len) - 1)) <<
                       (8 - s->r_len));
        *g = (uint8_t)(((v >> s->g_off) & ((1u << s->g_len) - 1)) <<
                       (8 - s->g_len));
        *b = (uint8_t)(((v >> s->b_off) & ((1u << s->b_len) - 1)) <<
                       (8 - s->b_len));
    } else {
        *r = *g = *b = 0;
    }
}

/* The art is self-opaque at #BFBFBF-plate colors, so it is copied straight
 * onto the panel (still honoring alpha < 255 for future assets). */
static void blit_gray(const screen_t *s, const sprite_t *sp, int ox, int oy)
{
    for (int y = 0; y < sp->h; y++) {
        int sy = oy + y;
        if (sy < 0 || sy >= s->h)
            continue;
        const uint8_t *row = sp->px + (size_t)y * (size_t)sp->w;
        for (int x = 0; x < sp->w; x++) {
            int sx = ox + x;
            if (sx < 0 || sx >= s->w)
                continue;
            uint8_t v = row[x];
            put_px(s, sx, sy, v, v, v, 0xff);
        }
    }
}

static void blit_rgba(const screen_t *s, const sprite_t *sp, int ox, int oy)
{
    for (int y = 0; y < sp->h; y++) {
        int sy = oy + y;
        if (sy < 0 || sy >= s->h)
            continue;
        const uint8_t *row = sp->px + (size_t)y * (size_t)sp->w * 4;
        for (int x = 0; x < sp->w; x++) {
            int sx = ox + x;
            if (sx < 0 || sx >= s->w)
                continue;
            const uint8_t *p = row + (size_t)x * 4;
            put_px(s, sx, sy, p[0], p[1], p[2], p[3]);
        }
    }
}

void bootpanel_draw(const screen_t *s, int frame)
{
    /* 1. background: #BFBFBF everywhere */
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++)
            put_px(s, x, y, BOOTPANEL_BG_R, BOOTPANEL_BG_G, BOOTPANEL_BG_B,
                   0xff);

    /* 2. logo, centered */
    sprite_t logo;
    logo.px = bootpanel_logo(&logo.w, &logo.h);
    blit_rgba(s, &logo, (s->w - logo.w) / 2, (s->h - logo.h) / 2);

    /* 3. spinner: x center of the display width, y = display height
     *    center + BOOTPANEL_SPINNER_OFFSET_Y px below, as spec'd. */
    sprite_t spin;
    spin.px = bootpanel_spinner(frame, &spin.w, &spin.h);
    blit_gray(s, &spin, (s->w - spin.w) / 2,
              s->h / 2 + BOOTPANEL_SPINNER_OFFSET_Y - spin.h / 2);
}
