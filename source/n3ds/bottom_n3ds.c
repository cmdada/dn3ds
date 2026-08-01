/*
 * bottom_n3ds.c - the second screen: a touch weapon/status panel.
 *
 * SDL_DUALSCR splits one surface: top half to the top screen, bottom half to
 * the touch screen. This writes 8-bit palette indices straight into the lower
 * half - same surface the game already draws into, just further down.
 *
 * Colours are matched against the live palette at runtime; Duke's palette
 * depends on the loaded GRP and shifts under the lighting code, so a magic
 * index is not stable where a nearest-RGB match is.
 */

#include <3ds.h>
#include <SDL.h>
#include <string.h>

#include "duke3d.h"
#include "keyboard.h"

extern SDL_Surface *surface;
extern uint8_t     *frameplace;
extern int32_t      bytesperline;
extern int32_t      ydim;

/* SDL_FITWIDTH draws the half 1:1 from x=0 onto the 320-wide touch screen, so
   only the left 320 columns are visible and touch coords land directly on
   surface pixels. */
#define PANEL_W       400
#define PANEL_H       240
#define VISIBLE_W     320

#define WPN_COLS      5
#define WPN_ROWS      2
#define WPN_COUNT     (WPN_COLS * WPN_ROWS)

#define BOX_W         56
#define BOX_H         50
#define BOX_GAP       5
#define GRID_X        12
#define GRID_Y        76

static int c_black, c_dim, c_frame, c_lit, c_text, c_health, c_armor, c_ammo;
static int palette_ready;

/* 5x7 digits, one byte per column, low bit at the top. */
static const unsigned char font_digits[10][5] = {
    {0x3e,0x51,0x49,0x45,0x3e}, {0x00,0x42,0x7f,0x40,0x00},
    {0x62,0x51,0x49,0x49,0x46}, {0x22,0x41,0x49,0x49,0x36},
    {0x18,0x14,0x12,0x7f,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1e},
};

static int nearest_color(int r, int g, int b)
{
    const SDL_Color *c;
    int best = 0, bestd = 1 << 30, i;

    if (!surface || !surface->format || !surface->format->palette)
        return 0;

    c = surface->format->palette->colors;
    for (i = 0; i < 256; i++) {
        int dr = c[i].r - r, dg = c[i].g - g, db = c[i].b - b;
        int d  = dr * dr + dg * dg + db * db;
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

static void build_palette(void)
{
    c_black  = nearest_color(0, 0, 0);
    c_dim    = nearest_color(40, 40, 44);
    c_frame  = nearest_color(120, 120, 128);
    c_lit    = nearest_color(230, 180, 60);
    c_text   = nearest_color(230, 230, 230);
    c_health = nearest_color(200, 40, 40);
    c_armor  = nearest_color(60, 140, 220);
    c_ammo   = nearest_color(230, 170, 40);
    palette_ready = 1;
}

/* ydim is already halved by display.c, so the engine's frame ends here. */
static uint8_t *panel_base(void)
{
    return frameplace + (ydim * bytesperline);
}

static void fill_rect(int x, int y, int w, int h, int col)
{
    uint8_t *base = panel_base();
    int iy;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > PANEL_W) w = PANEL_W - x;
    if (y + h > PANEL_H) h = PANEL_H - y;
    if (w <= 0 || h <= 0) return;

    for (iy = 0; iy < h; iy++)
        memset(base + (y + iy) * bytesperline + x, col, (size_t)w);
}

static void frame_rect(int x, int y, int w, int h, int col)
{
    fill_rect(x, y, w, 1, col);
    fill_rect(x, y + h - 1, w, 1, col);
    fill_rect(x, y, 1, h, col);
    fill_rect(x + w - 1, y, 1, h, col);
}

static void draw_digit(int x, int y, int d, int col, int scale)
{
    int cx, row;
    if (d < 0 || d > 9) return;
    for (cx = 0; cx < 5; cx++) {
        unsigned char bits = font_digits[d][cx];
        for (row = 0; row < 7; row++)
            if (bits & (1 << row))
                fill_rect(x + cx * scale, y + row * scale, scale, scale, col);
    }
}

static void draw_number(int x, int y, int value, int col, int scale)
{
    char buf[12];
    int n, i;

    if (value < 0) value = 0;
    n = snprintf(buf, sizeof(buf), "%d", value);
    for (i = 0; i < n; i++)
        draw_digit(x + i * 6 * scale, y, buf[i] - '0', col, scale);
}

static void draw_bar(int x, int y, int w, int h, int value, int maxv, int col)
{
    int filled;
    if (maxv <= 0) maxv = 1;
    if (value < 0) value = 0;
    if (value > maxv) value = maxv;
    filled = (w - 2) * value / maxv;

    frame_rect(x, y, w, h, c_frame);
    fill_rect(x + 1, y + 1, w - 2, h - 2, c_dim);
    if (filled > 0)
        fill_rect(x + 1, y + 1, filled, h - 2, col);
}

static void box_geometry(int i, int *x, int *y)
{
    *x = GRID_X + (i % WPN_COLS) * (BOX_W + BOX_GAP);
    *y = GRID_Y + (i / WPN_COLS) * (BOX_H + BOX_GAP);
}

void n3ds_bottom_draw(void)
{
    struct player_struct *p = &ps[myconnectindex];
    int i, health;

    if (!surface || !frameplace)
        return;
    if (!palette_ready)
        build_palette();

    fill_rect(0, 0, PANEL_W, PANEL_H, c_black);

    /* Health is on the sprite; last_extra is the status bar's lagged copy. */
    health = (p->i >= 0 && p->i < MAXSPRITES) ? sprite[p->i].extra : 0;

    /* Health and armour, with the numbers beside them. */
    draw_bar(12, 10, 120, 16, health, 100, c_health);
    draw_number(138, 12, health, c_text, 2);

    draw_bar(180, 10, 96, 16, p->shield_amount, 100, c_armor);
    draw_number(282, 12, p->shield_amount, c_text, 2);

    /* Ammo for whatever is in hand. */
    if (p->curr_weapon >= 0 && p->curr_weapon < MAX_WEAPONS) {
        draw_bar(12, 34, 120, 16, p->ammo_amount[p->curr_weapon], 200, c_ammo);
        draw_number(138, 36, p->ammo_amount[p->curr_weapon], c_text, 2);
    }


    for (i = 0; i < WPN_COUNT; i++) {
        int x, y, owned, current;

        box_geometry(i, &x, &y);
        owned   = (i < MAX_WEAPONS) && p->gotweapon[i];
        current = (p->curr_weapon == i);

        fill_rect(x, y, BOX_W, BOX_H, current ? c_lit : c_dim);
        frame_rect(x, y, BOX_W, BOX_H, owned ? c_frame : c_dim);


        draw_number(x + BOX_W / 2 - 9, y + BOX_H / 2 - 10,
                    (i == 9) ? 0 : i + 1,
                    current ? c_black : (owned ? c_text : c_frame), 3);
    }
}

/* Tapping a box presses that weapon's number key, so rebinding still works. */
void n3ds_bottom_touch(void)
{
    static int was_down;
    touchPosition touch;
    u32 held;
    int i, sx, sy, down;

    /* hidScanInput() already called by input_n3ds.c this frame. */
    held = hidKeysHeld();
    down = (held & KEY_TOUCH) != 0;

    for (i = 0; i < 9; i++)
        KB_KeyDown[sc_1 + i] = 0;
    KB_KeyDown[sc_0] = 0;

    if (!down) { was_down = 0; return; }

    hidTouchRead(&touch);


    sx = touch.px;
    sy = touch.py;

    for (i = 0; i < WPN_COUNT; i++) {
        int x, y;
        box_geometry(i, &x, &y);
        if (sx >= x && sx < x + BOX_W && sy >= y && sy < y + BOX_H) {
            KB_KeyDown[(i == 9) ? sc_0 : (sc_1 + i)] = 1;
            break;
        }
    }

    was_down = 1;
    (void)was_down;
}
