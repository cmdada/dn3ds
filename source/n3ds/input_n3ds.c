/*
 * input_n3ds.c - native 3DS control scheme.
 *
 * Synthesises KB_KeyDown[] from the 3DS hid rather than teaching control.c
 * about a gamepad: ACTION() and the menus both read that array, so feeding
 * the layer underneath the bindings keeps user rebinding working.
 */

#include <stdio.h>

#include <3ds.h>

#include "duke3d.h"
#include "keyboard.h"
#include "control.h"

/* Pad travel is ~+/-155 at the rim, but the corners never reach it. */
#define STICK_DEADZONE   24
#define STICK_RANGE     140
#define MOVE_SCALE       12

static u32 held;
static circlePosition circle;
static circlePosition cstick;

/* Test aid, gated on sdmc:/3ds/duke3d/autokey; inert without the file. */
extern void n3ds_force_key_waiting(int scancode);

static int autokey_enabled(void)
{
    static int checked, enabled;

    if (!checked) {
        FILE *f = fopen("sdmc:/3ds/duke3d/autokey", "rb");
        checked = 1;
        if (f) { enabled = 1; fclose(f); }
    }
    return enabled;
}

/* Called from _handle_events(), the only thing running inside the splash's
   `while(!KB_KeyWaiting());` spin. That loop reads a flag, not KB_KeyDown[],
   so it can only be broken by poking the flag. No frame window: the spin
   calls this thousands of times a second. */
void n3ds_autokey_tick(void)
{
    if (!autokey_enabled())
        return;
    n3ds_force_key_waiting(sc_Space);
}

static void keyset(int scancode, int down)
{
    KB_KeyDown[scancode] = down ? 1 : 0;
}

static int deadzone(int v)
{
    if (v > -STICK_DEADZONE && v < STICK_DEADZONE)
        return 0;
    if (v >  STICK_RANGE) return  STICK_RANGE;
    if (v < -STICK_RANGE) return -STICK_RANGE;
    return v;
}

static int in_menu(void)
{
    return (ps[myconnectindex].gm & MODE_MENU) != 0;
}

/*
 * Refresh KB_KeyDown from the pad. Called once per CONTROL_GetInput, which is
 * once per frame.
 */
void n3ds_input_update(void)
{
    int menu;
    int cx, cy, sx, sy;

    hidScanInput();
    held = hidKeysHeld();
    hidCircleRead(&circle);
    hidCstickRead(&cstick);

    menu = in_menu();

    cx = deadzone(circle.dx);
    cy = deadzone(circle.dy);
    sx = deadzone(cstick.dx);
    sy = deadzone(cstick.dy);

    /* Clear every key this layer owns: one left set across a mode change
       would stick down forever. */
    keyset(sc_UpArrow, 0);    keyset(sc_DownArrow, 0);
    keyset(sc_LeftArrow, 0);  keyset(sc_RightArrow, 0);
    keyset(sc_Return, 0);     keyset(sc_Escape, 0);
    keyset(sc_Space, 0);      keyset(sc_LeftControl, 0);
    keyset(sc_A, 0);          keyset(sc_Z, 0);
    keyset(sc_LeftShift, 0);  keyset(sc_Tab, 0);
    keyset(sc_Quote, 0);      keyset(sc_SemiColon, 0);
    keyset(sc_PgUp, 0);       keyset(sc_PgDn, 0);
    keyset(sc_Home, 0);       keyset(sc_End, 0);

    /* Test aid: the splash needs both the keyIsWaiting flag (poked by
       n3ds_autokey_tick) and a key actually down. */
    if (autokey_enabled()) {
        keyset(sc_Space, 1);
        keyset(sc_Return, 1);
        return;
    }

    if (menu) {
        keyset(sc_UpArrow,    (held & KEY_DUP)    || cy >  STICK_DEADZONE * 2);
        keyset(sc_DownArrow,  (held & KEY_DDOWN)  || cy < -STICK_DEADZONE * 2);
        keyset(sc_LeftArrow,  (held & KEY_DLEFT)  || cx < -STICK_DEADZONE * 2);
        keyset(sc_RightArrow, (held & KEY_DRIGHT) || cx >  STICK_DEADZONE * 2);

        keyset(sc_Return, held & KEY_A);
        keyset(sc_Escape, held & (KEY_B | KEY_START));
        return;
    }

    /* --- in the world ---------------------------------------------------- */

    keyset(sc_LeftControl, held & (KEY_ZR | KEY_R));   /* Fire            */
    keyset(sc_Space,       held & KEY_A);              /* Open / use      */
    keyset(sc_A,           held & KEY_B);              /* Jump            */
    keyset(sc_Z,           held & KEY_X);              /* Crouch          */
    keyset(sc_Return,      held & KEY_Y);              /* Use inventory   */
    keyset(sc_LeftShift,   held & KEY_ZL);             /* Run             */

    keyset(sc_Escape,      held & KEY_START);          /* Open the menu   */
    keyset(sc_Tab,         held & KEY_SELECT);         /* Overhead map    */


    keyset(sc_LeftArrow,   held & KEY_DLEFT);          /* Turn left       */
    keyset(sc_RightArrow,  held & KEY_DRIGHT);         /* Turn right      */
    keyset(sc_PgUp,        held & KEY_DUP);            /* Look up         */
    keyset(sc_PgDn,        held & KEY_DDOWN);          /* Look down       */

    /* Analog stick driving digital functions: the threshold must be well
       clear of the resting dead zone or an off-centre stick cycles weapons
       by itself. Aim_*, not Look_*, since the d-pad already owns look. */
    keyset(sc_Home,        sy >  STICK_DEADZONE * 2);  /* Aim up          */
    keyset(sc_End,         sy < -STICK_DEADZONE * 2);  /* Aim down        */
    keyset(sc_Quote,       sx >  STICK_DEADZONE * 2);  /* Next weapon     */
    keyset(sc_SemiColon,  (sx < -STICK_DEADZONE * 2)   /* Previous weapon */
                          || (held & KEY_L));
}

/* ControlInfo fields the mouse would normally drive. dz forward, dx strafe. */
void n3ds_input_analog(ControlInfo *info)
{
    int cx = deadzone(circle.dx);
    int cy = deadzone(circle.dy);

    if (in_menu())
        return;


    info->dz = cy * MOVE_SCALE;
    info->dx = cx * MOVE_SCALE;
}
