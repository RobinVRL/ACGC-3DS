#include "pc_platform.h"
#include "pc_pause_menu.h"
#include "pc_profiler.h"
#include "pc_settings.h"
#include "pc_model_viewer.h"
#include "pc_menu_util.h"
#include "Famicom/famicom.h"

#include "game.h"
#include "graph.h"
#include "m_font.h"

#include "acgc_3ds_platform.h"

#include <3ds.h>

int g_pc_running = 1;
int g_pc_verbose = 1;
int g_pc_frame_limit_override = 60;
int g_pc_speedhack_enabled = 0;
int g_pc_time_override = -1;
int g_pc_min_override = -1;
int g_pc_sec_override = -1;
int g_pc_date_month = -1;
int g_pc_date_day = -1;
int g_pc_date_year = -1;
int g_pc_weather_override = -1;
int g_pc_weather_intensity_override = 0;
int g_pc_window_w = 400;
int g_pc_window_h = 240;
int g_pc_widescreen_stretch = 0;
int g_pc_model_viewer = 0;
int g_pc_model_viewer_start = 0;
int g_pc_model_viewer_no_cull = 0;
int g_pc_paused = 0;
int g_pc_title_main_menu_visible = 0;
int g_pc_nes_active = 0;
int g_pc_profile_enabled = 0;
int g_pc_profile_interval = 0;
int g_pc_pause_input_drain = 0;
PCSettings g_pc_settings = {
    .window_width = 400, .window_height = 240, .max_fps = 60,
    .texture_filtering = 1, .master_volume = 100,
    .stick_deadzone = 12, .cstick_deadzone = 12
};
unsigned int pc_image_base = 0;
/* The 3DS image is below the N64 segmented-address window. Runtime pointers
 * are tagged by pc_gbi_pack_runtime_ptr, so no broad image-range exception is
 * needed here; treating the whole address space as the image breaks 0x03-0x0F
 * segment references such as the field renderer's segment A display list. */
unsigned int pc_image_end = 0;
Famicom_MallocInfo* my_malloc_current;
u8 save_game_image;
u16 s_tlut_first_word[16];

int pc_emu64_frame_cmds;
int pc_emu64_frame_noop_cmds;
int pc_emu64_frame_tri_cmds;
int pc_emu64_frame_vtx_cmds;
int pc_emu64_frame_dl_cmds;
int pc_emu64_frame_cull_visible;
int pc_emu64_frame_cull_rejected;
int pc_gx_draw_call_count;

enum {
    ACGC_3DS_OPTIONS_RESUME,
    ACGC_3DS_OPTIONS_EXIT,
    ACGC_3DS_OPTIONS_COUNT
};

static int s_3ds_options_menu_active;
static int s_3ds_options_menu_selection;

static void pc_3ds_options_menu_close(void) {
    s_3ds_options_menu_active = 0;
    g_pc_paused = 0;
    g_pc_pause_input_drain = 1;
}

void pc_platform_init(void) {
    g_pc_running = acgc_3ds_platform_init();
    if (g_pc_running) acgc_3ds_platform_begin_frame();
}
void pc_platform_shutdown(void) {
    pc_audio_shutdown();
    acgc_3ds_platform_shutdown();
    g_pc_running = 0;
}
void pc_platform_update_window_size(void) {}
void pc_platform_swap_buffers(void) {
    extern void pc_gx_draw_pending(void);

    pc_gx_draw_pending();
    acgc_3ds_platform_end_frame();
    if (g_pc_running) acgc_3ds_platform_begin_frame();
}
int pc_platform_poll_events(void) {
    g_pc_running = aptMainLoop();
    if (!g_pc_running) return 0;

    {
        /* acgc_3ds_platform_begin_frame owns the one HID scan for this frame.
         * Re-scanning here would clear libctru's edge-sensitive keysDown. */
        u32 down = acgc_3ds_input_keys_down();
        if (down & KEY_SELECT) {
            pc_pause_menu_toggle();
            return g_pc_running;
        }
        if (s_3ds_options_menu_active) {
            if (down & (KEY_DLEFT | KEY_DDOWN)) {
                s_3ds_options_menu_selection--;
                if (s_3ds_options_menu_selection < 0) {
                    s_3ds_options_menu_selection = ACGC_3DS_OPTIONS_COUNT - 1;
                }
            }
            if (down & (KEY_DRIGHT | KEY_DUP)) {
                s_3ds_options_menu_selection++;
                if (s_3ds_options_menu_selection >= ACGC_3DS_OPTIONS_COUNT) {
                    s_3ds_options_menu_selection = 0;
                }
            }
            if (down & KEY_A) {
                if (s_3ds_options_menu_selection == ACGC_3DS_OPTIONS_EXIT) {
                    pc_3ds_options_menu_close();
                    g_pc_running = 0;
                } else {
                    pc_3ds_options_menu_close();
                }
            }
            if (down & KEY_B) pc_3ds_options_menu_close();
        }
    }
    return g_pc_running;
}

void pc_pause_menu_toggle(void) {
    if (s_3ds_options_menu_active) {
        pc_3ds_options_menu_close();
        return;
    }
    if (g_pc_nes_active) return;
    s_3ds_options_menu_selection = ACGC_3DS_OPTIONS_RESUME;
    s_3ds_options_menu_active = 1;
    g_pc_paused = 1;
}
int pc_pause_menu_handle_event(const SDL_Event* event) { (void)event; return 0; }
void pc_pause_menu_draw(struct game_s* game) {
    int resume_selected;
    int exit_selected;

    if (!s_3ds_options_menu_active || game == NULL || game->graph == NULL) return;

    resume_selected = s_3ds_options_menu_selection == ACGC_3DS_OPTIONS_RESUME;
    exit_selected = s_3ds_options_menu_selection == ACGC_3DS_OPTIONS_EXIT;

    OPEN_DISP(game->graph);
    gDPNoOpTag(NEXT_FONT_DISP, PC_NOOP_3DS_SCREEN_BOTTOM);
    CLOSE_DISP(game->graph);

    mFont_SetMatrix(game->graph, mFont_MODE_FONT);
    pc_menu_dim_rect(game->graph, 210);
    pc_menu_draw_centered(game, "3DS OPTIONS", 48.0f, 255, 235, 120, 255, 1.15f);
    pc_menu_draw_centered(game, resume_selected ? ">  RESUME  <" : "   RESUME   ",
                          94.0f,
                          resume_selected ? 255 : 185,
                          resume_selected ? 235 : 185,
                          resume_selected ? 120 : 185,
                          255, resume_selected ? 1.0f : 0.9f);
    pc_menu_draw_centered(game, exit_selected ? ">  EXIT GAME  <" : "   EXIT GAME   ",
                          134.0f,
                          exit_selected ? 255 : 185,
                          exit_selected ? 145 : 185,
                          exit_selected ? 120 : 185,
                          255, exit_selected ? 1.0f : 0.9f);
    pc_menu_draw_centered(game, "D-PAD  choose     A  select", 178.0f,
                          210, 210, 210, 230, 0.8f);
    pc_menu_draw_centered(game, "B / SELECT  close", 207.0f,
                          210, 210, 210, 230, 0.8f);
    mFont_UnSetMatrix(game->graph, mFont_MODE_FONT);

    OPEN_DISP(game->graph);
    gDPNoOpTag(NEXT_FONT_DISP, PC_NOOP_3DS_SCREEN_TOP);
    CLOSE_DISP(game->graph);
}
int pc_settings_menu_active(void) { return s_3ds_options_menu_active; }
void pc_model_viewer_init(GAME* game) { (void)game; }
void pc_model_viewer_cleanup(GAME* game) { (void)game; }

int famicom_getErrorChan(void) { return -1; }
void famicom_setCallback_getSaveChan(FAMICOM_GETSAVECHAN_PROC proc) { (void)proc; }
int famicom_mount_archive_end_check(void) { return TRUE; }
void famicom_mount_archive(void) {}
int famicom_init(int rom_idx, Famicom_MallocInfo* info, int player_no) {
    (void)rom_idx; (void)info; (void)player_no; return -1;
}
int famicom_cleanup(void) { return 0; }
void famicom_1frame(void) {}
int famicom_rom_load_check(void) { return -1; }
int famicom_internal_data_load(void) { return FAMICOM_RESULT_NOENTRY; }
int famicom_internal_data_save(void) { return FAMICOM_RESULT_WRONGDEVICE; }
int famicom_external_data_save(void) { return FAMICOM_RESULT_WRONGDEVICE; }
int famicom_external_data_save_check(void) { return FAMICOM_RESULT_NOENTRY; }
int famicom_get_disksystem_titles(int* count, char* names, int size) {
    (void)names; (void)size; if (count) *count = 0; return FALSE;
}

void pc_profiler_begin_frame(void) {}
void pc_profiler_end_frame(double frame_ms, int audio_fill) {
    (void)frame_ms;
    (void)audio_fill;
}
void pc_profiler_add_time_slow(PCProfilerTimer timer, Uint64 start) { (void)timer; (void)start; }
void pc_profiler_add_count_draw_slow(int vertices, int indices) { (void)vertices; (void)indices; }
void pc_profiler_add_count_flush_slow(void) {}
void pc_profiler_add_count_shader_switch_slow(void) {}
void pc_profiler_add_count_uniform_slow(void) {}
void pc_profiler_add_count_uniform_lookup_slow(void) {}
void pc_profiler_add_count_texture_bind_slow(void) {}
void pc_profiler_add_count_buffer_upload_slow(size_t bytes) { (void)bytes; }
void pc_profiler_add_count_state_change_slow(void) {}
void pc_profiler_add_dirty_mask_slow(unsigned int dirty) { (void)dirty; }
