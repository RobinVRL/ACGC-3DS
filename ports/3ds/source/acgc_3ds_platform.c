#include "acgc_3ds_platform.h"
#include "acgc_3ds_texture_pack.h"

#include <string.h>

int acgc_3ds_audio_init(void);
int acgc_3ds_audio_ready(void);
void acgc_3ds_audio_shutdown(void);
void acgc_3ds_input_scan(void);
void acgc_3ds_input_end_frame(void);
void acgc_3ds_input_get(AcgcPadStatus* out);
int acgc_3ds_save_init(void);
int acgc_3ds_save_ready(void);
int acgc_3ds_video_init(void);
int acgc_3ds_video_ready(void);
void acgc_3ds_video_begin_frame(void);
void acgc_3ds_video_end_frame(void);
void acgc_3ds_video_shutdown(void);

int acgc_3ds_platform_init(void) {
    acgc_3ds_texture_pack_init();
    if (!acgc_3ds_video_init()) {
        acgc_3ds_texture_pack_shutdown();
        return 0;
    }
    if (!acgc_3ds_save_init()) {
        acgc_3ds_video_shutdown();
        acgc_3ds_texture_pack_shutdown();
        return 0;
    }

    /* NDSP can be unavailable when DSP firmware is missing or an emulator
     * does not expose the service. The audio backend already treats that as a
     * silent/no-op device, so it must not prevent the game from launching. */
    (void)acgc_3ds_audio_init();
    return 1;
}

void acgc_3ds_platform_begin_frame(void) {
    acgc_3ds_input_scan();
    acgc_3ds_video_begin_frame();
}

void acgc_3ds_platform_end_frame(void) {
    acgc_3ds_video_end_frame();
    acgc_3ds_input_end_frame();
}

void acgc_3ds_platform_shutdown(void) {
    acgc_3ds_audio_shutdown();
    acgc_3ds_video_shutdown();
    acgc_3ds_texture_pack_shutdown();
}

void acgc_3ds_platform_get_status(Acgc3dsPlatformStatus* out) {
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->video_ready = acgc_3ds_video_ready();
    out->audio_ready = acgc_3ds_audio_ready();
    out->save_ready = acgc_3ds_save_ready();
    acgc_3ds_input_get(&out->pad);
}
