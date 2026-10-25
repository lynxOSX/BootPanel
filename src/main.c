/*
 * BootPanel — the lynxOS boot panel (a plymouth-alike).
 *
 * Draws the lynxOS logo centered on the display and a 12-frame spinner
 * centered horizontally, its center 185 px below the display height center,
 * over a hardcoded #BFBFBF background, until stopped (SIGINT/SIGTERM).
 *
 * Backend: KMS/DRM when available (double buffered, page flips), else the
 * legacy fbdev interface. Assets are baked into the binary at compile time
 * (src/assets_embed.c generated from assets/ by tools/genassets.py), so no
 * filesystem or libpng is needed at runtime.
 *
 * MIT — see LICENSE.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bootpanel.h"

#define DEFAULT_INTERVAL 100 /* ms per spinner frame: 10 fps */
#define SHOT_W 1920
#define SHOT_H 1080

static volatile sig_atomic_t stop_requested;

static void on_stop(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
"usage: %s [options]\n"
"\n"
"Show the lynxOS boot panel on the best display backend available and\n"
"animate the spinner until a stop signal arrives:\n"
"  --kms               force the KMS/DRM backend (builds with libdrm)\n"
"  --fbdev             force the legacy fbdev backend\n"
"  --device=PATH       device for the forced backend (card0/fb0 by default)\n"
"  --interval=MS       ms per spinner frame, 1..1000 (default %d = 10 fps)\n"
"  --screenshot=PATH   compose one frame (no display needed) as a PPM, exit\n"
"  --width=N           screenshot width, points (default %d)\n"
"  --height=N          screenshot height, points (default %d)\n"
"  --frame=N           spinner frame for --screenshot (default 0)\n"
"  --help\n",
            prog, DEFAULT_INTERVAL, SHOT_W, SHOT_H);
}

static void write_ppm(const char *path, const screen_t *s)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "bootpanel: cannot write %s: %s\n", path,
                strerror(errno));
        exit(1);
    }
    fprintf(f, "P6\n%d %d 255\n", s->w, s->h);
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            uint8_t rgb[3];
            screen_get_px(s, x, y, &rgb[0], &rgb[1], &rgb[2]);
            fwrite(rgb, 1, 3, f);
        }
    if (fclose(f) != 0) {
        fprintf(stderr, "bootpanel: writing %s failed: %s\n", path,
                strerror(errno));
        exit(1);
    }
}

/* compose one frame into an in-memory screen and write it out as a PPM */
static int screenshots(int argc, char **argv)
{
    const char *out = NULL;
    int w = SHOT_W, h = SHOT_H, frame = 0;

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--screenshot=", 13) == 0)
            out = a + 13;
        else if (strncmp(a, "--width=", 8) == 0)
            w = atoi(a + 8);
        else if (strncmp(a, "--height=", 9) == 0)
            h = atoi(a + 9);
        else if (strncmp(a, "--frame=", 8) == 0)
            frame = atoi(a + 8);
    }
    if (!out)
        return 0;

    screen_t s;
    memset(&s, 0, sizeof(s));
    s.w = w > 0 ? w : SHOT_W;
    s.h = h > 0 ? h : SHOT_H;

    uint8_t *mem = malloc((size_t)s.h * s.w * 3);
    if (!mem) {
        fprintf(stderr, "bootpanel: out of memory\n");
        return 1;
    }
    s.map = mem;
    s.stride = s.w * 3;
    s.bpp = 24;
    s.r_off = 0;
    s.g_off = 8;
    s.b_off = 16;
    s.r_len = s.g_len = s.b_len = 8;

    bootpanel_draw(&s, frame);
    write_ppm(out, &s);
    free(mem);
    return 1;
}

int main(int argc, char **argv)
{
    bool force_kms = false, force_fbdev = false;
    const char *device = NULL;
    int interval = DEFAULT_INTERVAL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--kms") == 0)
            force_kms = true;
        else if (strcmp(a, "--fbdev") == 0)
            force_fbdev = true;
        else if (strncmp(a, "--device=", 9) == 0)
            device = a + 9;
        else if (strncmp(a, "--interval=", 11) == 0) {
            interval = atoi(a + 11);
            if (interval < 1 || interval > 1000) {
                fprintf(stderr, "bootpanel: bad --interval=%d\n", interval);
                return 2;
            }
        } else if (strncmp(a, "--screenshot=", 13) == 0 ||
                   strncmp(a, "--width=", 8) == 0 ||
                   strncmp(a, "--height=", 9) == 0 ||
                   strncmp(a, "--frame=", 8) == 0) {
            /* handled below by screenshots() */
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout, argv[0]);
            return 0;
        } else {
            fprintf(stderr, "bootpanel: unknown argument %s\n", a);
            usage(stderr, argv[0]);
            return 2;
        }
    }

    if (screenshots(argc, argv))
        return 0;

    if (force_kms) {
#ifndef HAVE_KMS
        fprintf(stderr, "bootpanel: not built with KMS/DRM support\n");
        return 1;
#endif
    }

    /* candidate order: kms (when built/allowed), then legacy fbdev */
    backend_t *cand[2];
    int ncand = 0;
#ifndef HAVE_KMS
    if (force_kms)
        return 1;
#else
    if (!force_fbdev) {
        backend_t *k = backend_kms();
        if (k)
            cand[ncand++] = k;
        else if (force_kms) {
            fprintf(stderr, "bootpanel: KMS/DRM backend unavailable\n");
            return 1;
        }
    } else if (force_kms) {
        fprintf(stderr, "bootpanel: --kms and --fbdev are mutually exclusive\n");
        return 2;
    }
#endif
    if (!force_kms) {
        backend_t *f = backend_fbdev();
        if (f)
            cand[ncand++] = f;
    }

    backend_t *b = NULL;
    for (int i = 0; i < ncand; i++) {
        if (cand[i]->open(cand[i], device) == 0) {
            b = cand[i];
            break;
        }
        cand[i]->close(cand[i]);
    }
    if (!b) {
        fprintf(stderr, "bootpanel: no display backend available\n");
        return 1;
    }

    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);

    fprintf(stderr, "bootpanel: backend %s\n", b->name);
    textconsole_inhibit();

    long frame_us = (long)interval * 1000L;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    long frame = 0;

    while (!stop_requested) {
        frame = (next.tv_sec * 1000L + next.tv_nsec / 1000000L) / interval;
        const screen_t *s = b->acquire(b);
        if (s)
            bootpanel_draw(s, (int)(frame % BOOTPANEL_SPINNER_FRAMES));
        b->present(b);

        next.tv_nsec += frame_us;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_sec++;
            next.tv_nsec -= 1000000000L;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        /* if we drifted behind (slow buffer), catch up */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long ahead = (long)(now.tv_nsec - next.tv_nsec) +
                     (long)(now.tv_sec - next.tv_sec) * 1000000000L;
        while (ahead >= 0) {
            next.tv_nsec += frame_us;
            while (next.tv_nsec >= 1000000000L) {
                next.tv_sec++;
                next.tv_nsec -= 1000000000L;
            }
            ahead -= frame_us;
        }
    }

    textconsole_restore();
    b->close(b);
    return 0;
}
