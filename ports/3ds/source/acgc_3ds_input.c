#include "acgc_3ds_platform.h"

#include <3ds.h>
#include <string.h>

static AcgcPadStatus g_pad;
static u32 g_keys_down;
static u32 g_keys_held;
static u32 g_touch_action;
static u32 g_touch_key_down_latch;
static u16 g_touch_button_latch;

static u32 touch_action_at(u16 x, u16 y) {
    int dx;
    int dy;

    if (y >= 38 && y < 136) {
        if (x >= 12 && x < 104) return ACGC_3DS_TOUCH_MAP;
        if (x >= 114 && x < 206) return ACGC_3DS_TOUCH_POCKETS;
        if (x >= 216 && x < 308) return ACGC_3DS_TOUCH_OPTIONS;
    }

    if (x < 12 || x >= 308 || y < 148 || y >= 232) return ACGC_3DS_TOUCH_NONE;
    dx = (int)x - 160;
    dy = (int)y - 190;
    if (dx * dx + dy * dy < 14 * 14) return ACGC_3DS_TOUCH_NONE;
    if (dx < 0 && -dx > (dy < 0 ? -dy : dy)) return ACGC_3DS_TOUCH_LOOK_LEFT;
    if (dx > 0 && dx > (dy < 0 ? -dy : dy)) return ACGC_3DS_TOUCH_LOOK_RIGHT;
    return dy < 0 ? ACGC_3DS_TOUCH_LOOK_UP : ACGC_3DS_TOUCH_LOOK_DOWN;
}

static u16 touch_action_pad_button(u32 action) {
    /* These are the GameCube buttons consumed by PADRead. The game converts
     * them to BUTTON_X/BUTTON_Y before m_submenu checks its Map/Pockets
     * shortcuts. Keep this semantic mapping independent of libctru key bits. */
    if (action == ACGC_3DS_TOUCH_MAP) return ACGC_PAD_BUTTON_X;
    if (action == ACGC_3DS_TOUCH_POCKETS) return ACGC_PAD_BUTTON_Y;
    return 0;
}

static s8 clamp_stick(int value) {
    if (value > 127) {
        return 127;
    }
    if (value < -128) {
        return -128;
    }
    return (s8)value;
}

void acgc_3ds_input_scan(void) {
    circlePosition circle;
    circlePosition cstick;
    touchPosition touch;
    u32 held;
    u32 previous_touch_action = g_touch_action;
    u16 touch_button;
    u16 buttons = 0;

    hidScanInput();
    held = hidKeysHeld();
    g_keys_down = hidKeysDown();
    hidCircleRead(&circle);
    hidCstickRead(&cstick);
    g_touch_action = ACGC_3DS_TOUCH_NONE;
    if (held & KEY_TOUCH) {
        hidTouchRead(&touch);
        g_touch_action = touch_action_at(touch.px, touch.py);
    }
    touch_button = touch_action_pad_button(g_touch_action);

    /* Options is a platform menu action, not a GameCube button. Map and
     * Pockets are injected below as explicit GC X/Y buttons instead of
     * relying on KEY_X/KEY_Y happening to share the same bit positions. */
    g_keys_held = held;
    if (g_touch_action == ACGC_3DS_TOUCH_OPTIONS) {
        g_keys_held |= KEY_SELECT;
        if (g_touch_action != previous_touch_action) {
            g_touch_key_down_latch |= KEY_SELECT;
        }
    }
    g_keys_down |= g_touch_key_down_latch;

    /* A stylus tap can begin and end between two PADRead calls even though it
     * was observed by HID. Retain card-button edges until the current frame is
     * presented; all PADRead calls in that frame then observe the same pulse. */
    if (touch_button != 0 && g_touch_action != previous_touch_action) {
        g_touch_button_latch |= touch_button;
    }

    if (held & KEY_A) {
        buttons |= ACGC_PAD_BUTTON_A;
    }
    if (held & KEY_B) {
        buttons |= ACGC_PAD_BUTTON_B;
    }
    if (held & KEY_X) {
        buttons |= ACGC_PAD_BUTTON_X;
    }
    if (held & KEY_Y) {
        buttons |= ACGC_PAD_BUTTON_Y;
    }
    if (held & KEY_START) {
        buttons |= ACGC_PAD_BUTTON_START;
    }
    if (held & KEY_L) {
        buttons |= ACGC_PAD_TRIGGER_L;
    }
    if (held & KEY_R) {
        buttons |= ACGC_PAD_TRIGGER_R;
    }
    if (held & KEY_ZL) {
        buttons |= ACGC_PAD_TRIGGER_Z;
    }
    if (held & KEY_DUP) {
        buttons |= ACGC_PAD_BUTTON_UP;
    }
    if (held & KEY_DDOWN) {
        buttons |= ACGC_PAD_BUTTON_DOWN;
    }
    if (held & KEY_DLEFT) {
        buttons |= ACGC_PAD_BUTTON_LEFT;
    }
    if (held & KEY_DRIGHT) {
        buttons |= ACGC_PAD_BUTTON_RIGHT;
    }

    memset(&g_pad, 0, sizeof(g_pad));
    g_pad.button = buttons | touch_button | g_touch_button_latch;
    g_pad.stick_x = clamp_stick(circle.dx * 127 / 156);
    g_pad.stick_y = clamp_stick(circle.dy * 127 / 156);
    g_pad.cstick_x = clamp_stick(cstick.dx * 127 / 156);
    g_pad.cstick_y = clamp_stick(cstick.dy * 127 / 156);
    if (g_touch_action == ACGC_3DS_TOUCH_LOOK_LEFT) g_pad.cstick_x = -96;
    if (g_touch_action == ACGC_3DS_TOUCH_LOOK_RIGHT) g_pad.cstick_x = 96;
    if (g_touch_action == ACGC_3DS_TOUCH_LOOK_UP) g_pad.cstick_y = 96;
    if (g_touch_action == ACGC_3DS_TOUCH_LOOK_DOWN) g_pad.cstick_y = -96;
    g_pad.trigger_left = (held & KEY_L) ? 255 : 0;
    g_pad.trigger_right = (held & KEY_R) ? 255 : 0;
}

void acgc_3ds_input_get(AcgcPadStatus* out) {
    if (out != NULL) {
        *out = g_pad;
    }
}

u32 acgc_3ds_input_keys_down(void) {
    return g_keys_down;
}

u32 acgc_3ds_input_keys_held(void) {
    return g_keys_held;
}

u32 acgc_3ds_input_touch_action(void) {
    return g_touch_action;
}

void acgc_3ds_input_end_frame(void) {
    g_touch_key_down_latch = 0;
    g_touch_button_latch = 0;
}
