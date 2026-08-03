#include "acgc_3ds_platform.h"

#include <3ds.h>
#include <string.h>

static AcgcPadStatus g_pad;
static u32 g_keys_down;
static u32 g_keys_held;

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
    u32 held;
    u16 buttons = 0;

    hidScanInput();
    held = hidKeysHeld();
    g_keys_down = hidKeysDown();
    g_keys_held = held;
    hidCircleRead(&circle);
    hidCstickRead(&cstick);

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
    g_pad.button = buttons;
    g_pad.stick_x = clamp_stick(circle.dx * 127 / 156);
    g_pad.stick_y = clamp_stick(circle.dy * 127 / 156);
    g_pad.cstick_x = clamp_stick(cstick.dx * 127 / 156);
    g_pad.cstick_y = clamp_stick(cstick.dy * 127 / 156);
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
