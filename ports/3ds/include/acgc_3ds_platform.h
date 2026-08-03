#ifndef ACGC_3DS_PLATFORM_H
#define ACGC_3DS_PLATFORM_H

#include <stddef.h>

#include "acgc_3ds_disc.h"

enum {
    ACGC_PAD_BUTTON_LEFT  = 0x0001,
    ACGC_PAD_BUTTON_RIGHT = 0x0002,
    ACGC_PAD_BUTTON_DOWN  = 0x0004,
    ACGC_PAD_BUTTON_UP    = 0x0008,
    ACGC_PAD_TRIGGER_Z    = 0x0010,
    ACGC_PAD_TRIGGER_R    = 0x0020,
    ACGC_PAD_TRIGGER_L    = 0x0040,
    ACGC_PAD_BUTTON_A     = 0x0100,
    ACGC_PAD_BUTTON_B     = 0x0200,
    ACGC_PAD_BUTTON_X     = 0x0400,
    ACGC_PAD_BUTTON_Y     = 0x0800,
    ACGC_PAD_BUTTON_START = 0x1000
};

typedef struct AcgcPadStatus {
    u16 button;
    s8 stick_x;
    s8 stick_y;
    s8 cstick_x;
    s8 cstick_y;
    u8 trigger_left;
    u8 trigger_right;
    s8 err;
} AcgcPadStatus;

typedef struct Acgc3dsPlatformStatus {
    int video_ready;
    int audio_ready;
    int save_ready;
    AcgcPadStatus pad;
} Acgc3dsPlatformStatus;

typedef struct Acgc3dsGpuVertex {
    float x, y, z;
    float s, t;
    float s1, t1;
    float r, g, b, a;
} Acgc3dsGpuVertex;

typedef enum Acgc3dsTevPreset {
    ACGC_3DS_TEV_MODULATE,
    ACGC_3DS_TEV_DECAL,
    ACGC_3DS_TEV_BLEND,
    ACGC_3DS_TEV_REPLACE,
    ACGC_3DS_TEV_PASSCLR,
    ACGC_3DS_TEV_SPOTLIGHT,
    ACGC_3DS_TEV_MASKED,
    ACGC_3DS_TEV_FONT_MASK,
    ACGC_3DS_TEV_HUD_TINT,
    ACGC_3DS_TEV_HUD_GRADIENT,
    ACGC_3DS_TEV_FALLBACK
} Acgc3dsTevPreset;

typedef enum Acgc3dsRenderLayer {
    ACGC_3DS_RENDER_WORLD = 0,
    ACGC_3DS_RENDER_UI = 1
} Acgc3dsRenderLayer;

int acgc_3ds_platform_init(void);
void acgc_3ds_platform_begin_frame(void);
void acgc_3ds_platform_end_frame(void);
void acgc_3ds_platform_shutdown(void);
void acgc_3ds_platform_get_status(Acgc3dsPlatformStatus* out);
void acgc_3ds_input_get(AcgcPadStatus* out);
u32 acgc_3ds_input_keys_down(void);
u32 acgc_3ds_input_keys_held(void);
void acgc_3ds_video_enable_game_screens(void);
int acgc_3ds_video_get_render_scale(void);
void acgc_3ds_video_set_render_scale(int percent);
int acgc_3ds_video_draw_triangles(const Acgc3dsGpuVertex* vertices, size_t count);
int acgc_3ds_video_draw_gx_triangles(const Acgc3dsGpuVertex* vertices, size_t count,
                                     const float projection[4][4],
                                     const float modelview[4][4],
                                     const void* texture0,
                                     const void* texture1,
                                     u32 tev_color1,
                                     u32 tev_color2,
                                     Acgc3dsTevPreset tev_preset,
                                     Acgc3dsRenderLayer layer,
                                     const float viewport[6],
                                     int blend_enable,
                                     int depth_test,
                                     int depth_func,
                                     int depth_write);
int acgc_3ds_audio_init(void);
int acgc_3ds_audio_ready(void);
void acgc_3ds_audio_shutdown(void);
int acgc_3ds_audio_submit(const void* samples, size_t byte_count, int volume);
int acgc_3ds_audio_buffered_samples(void);

#endif
