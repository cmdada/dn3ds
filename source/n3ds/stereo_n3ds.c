/* stereo_n3ds.c - stereoscopic 3D on the top screen. */

#include <stdlib.h>
#include <string.h>

#include <3ds.h>
#include <SDL.h>

#include "duke3d.h"

/* Added to libSDL by tools/patch_sdl.py. Negative means the driver could not
   build the right eye. */
extern int SDL_N3DS_SetStereoBuffer(void *paletted_right);

extern SDL_Surface *surface;
extern int32_t      bytesperline;
extern int32_t      ydim;

/* A doorway is ~1024 units, so real 65mm separation is ~66. Slightly above
   life size; 768 was tried and is far too much to fuse. */
#define EYE_SEPARATION_MAX 96

/* Convergence, in pixels of horizontal image shift at full slider.
   The cameras are parallel, so without this every disparity has the same
   sign and nothing sits at the screen plane - which the eye reads as one
   shifted image rather than as depth. Shifting the right eye puts a mid
   distance at zero parallax, so nearer geometry comes forward and the far
   skyline sits back. */
#define CONVERGENCE_PX_MAX 5

static uint8_t *right_surface;
static uint8_t *left_keep;
static size_t   surface_bytes;
static int      surface_h;
static int      stereo_ready;
static int      have_right;
static int      stereo_refused;

int n3ds_stereo_init(void)
{
    if (stereo_ready)
        return 1;
    if (!surface)
        return 0;

    surface_h = surface->h;
    surface_bytes = (size_t)bytesperline * surface_h;

    right_surface = malloc(surface_bytes);
    left_keep     = malloc(surface_bytes);
    if (!right_surface || !left_keep) {
        free(right_surface); free(left_keep);
        right_surface = left_keep = NULL;
        return 0;
    }
    memset(right_surface, 0, surface_bytes);
    memset(left_keep, 0, surface_bytes);

    stereo_ready = 1;
    return 1;
}

static float slider_state(void)
{
    float slider;

    if (stereo_refused)
        return 0.0f;
    slider = osGet3DSliderState();
    return (slider <= 0.02f) ? 0.0f : slider;
}

static int eye_offset(void)
{
    return (int)(slider_state() * EYE_SEPARATION_MAX);
}

static int convergence_px(void)
{
    return (int)(slider_state() * CONVERGENCE_PX_MAX);
}

void n3ds_stereo_render_right(int32_t cposx, int32_t cposy, int32_t cposz,
                              short cang, int32_t choriz, short sect,
                              int32_t smoothratio)
{
    int off = eye_offset();
    int32_t ex, ey;
    int y, y0, y1, sh;
    size_t span;

    have_right = 0;
    if (!stereo_ready || off == 0)
        return;

    /* Strafe vector: right is (sin(a), -cos(a)). */
    ex = (off * sintable[cang & 2047]) >> 14;
    ey = (-off * sintable[(cang + 512) & 2047]) >> 14;

    y0 = windowy1 < 0 ? 0 : windowy1;
    y1 = windowy2 > ydim - 1 ? ydim - 1 : windowy2;
    if (y1 < y0)
        return;
    span = (size_t)bytesperline;

    /* Do NOT try to redirect frameplace to a private buffer: the renderer
       caches pointers derived from it when a frame starts, so the second pass
       draws into the real surface regardless and the buffer keeps a copy of
       the left eye. Stash the left viewport, draw, lift the result out,
       restore. */
    for (y = y0; y <= y1; y++)
        memcpy(left_keep + (size_t)y * span,
               (uint8_t *)surface->pixels + (size_t)y * span, span);

    drawrooms(cposx + ex, cposy + ey, cposz, cang, choriz, sect);
    animatesprites(cposx + ex, cposy + ey, cang, smoothratio);
    drawmasks();

    sh = convergence_px();
    for (y = y0; y <= y1; y++) {
        uint8_t       *dst = right_surface + (size_t)y * span;
        const uint8_t *src = (uint8_t *)surface->pixels + (size_t)y * span;

        /* Shift right by sh, replicating the leftmost column into the gap. */
        if (sh > 0 && sh < (int)span) {
            memcpy(dst + sh, src, span - sh);
            memset(dst, src[0], (size_t)sh);
        } else {
            memcpy(dst, src, span);
        }
        memcpy((uint8_t *)surface->pixels + (size_t)y * span,
               left_keep + (size_t)y * span, span);
    }

    have_right = 1;
}

void n3ds_stereo_present(void)
{
    const uint8_t *left;
    int y, x1, x2, tail, rc;

    if (!stereo_ready || stereo_refused)
        return;

    if (!have_right) {
        SDL_N3DS_SetStereoBuffer(NULL);
        return;
    }

    left = (const uint8_t *)surface->pixels;
    x1 = windowx1;
    x2 = windowx2;
    if (x1 < 0) x1 = 0;
    if (x2 > bytesperline - 1) x2 = bytesperline - 1;
    tail = bytesperline - (x2 + 1);

    /* Only the 3D viewport differs between eyes; everything else comes from
       the finished left frame so it sits at screen depth. */
    for (y = 0; y < surface_h; y++) {
        uint8_t *dst = right_surface + (size_t)y * bytesperline;
        const uint8_t *src = left + (size_t)y * bytesperline;

        if (y < windowy1 || y > windowy2) {
            memcpy(dst, src, (size_t)bytesperline);
        } else {
            const uint8_t *world = left_keep + (size_t)y * bytesperline;
            int x;

            if (x1 > 0)
                memcpy(dst, src, (size_t)x1);
            if (tail > 0)
                memcpy(dst + x2 + 1, src + x2 + 1, (size_t)tail);

            /* The weapon, crosshair and any messages are drawn by
               displayrest() *after* the right eye was captured, so they exist
               only in the left frame. left_keep still holds the left world at
               that same moment, so anything differing from it is one of those
               overlays: copy those pixels across, which also puts them at
               screen depth rather than at the world's parallax. */
            for (x = x1; x <= x2; x++)
                if (src[x] != world[x])
                    dst[x] = src[x];
        }
    }

    rc = SDL_N3DS_SetStereoBuffer(right_surface);
    if (rc < 0) {
        stereo_refused = 1;
        have_right = 0;
    }
}
