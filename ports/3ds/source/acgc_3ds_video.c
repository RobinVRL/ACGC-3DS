#include "acgc_3ds_platform.h"

#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <string.h>

#include "acgc_3ds_vshader_shbin.h"

#define ACGC_3DS_MAX_FRAME_VERTICES 32768u
#define ACGC_3DS_SCENE_TEX_W 512
#define ACGC_3DS_SCENE_TEX_H 256
#define ACGC_3DS_UI_W 320
#define ACGC_3DS_UI_H 240
#define ACGC_3DS_WORLD_W 400
#define ACGC_3DS_WORLD_H 240

#define ACGC_DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | \
     GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static C3D_RenderTarget* g_top;
static C3D_RenderTarget* g_bottom;
static C3D_RenderTarget* g_scene;
static C3D_RenderTarget* g_ui;
static C3D_Tex g_scene_texture;
static C3D_Tex g_ui_texture;
static int g_scene_texture_ready;
static int g_ui_texture_ready;
static int g_bottom_enabled;
static int g_video_ready;
static int g_frame_active;

static DVLB_s* g_shader_dvlb;
static shaderProgram_s g_shader_program;
static int g_shader_program_ready;
static C3D_Mtx g_projection;
static C3D_Mtx g_modelview;
static C3D_Mtx g_sent_projection;
static C3D_Mtx g_sent_modelview;
static int g_projection_uniform;
static int g_modelview_uniform;
static int g_sent_projection_valid;
static int g_sent_modelview_valid;

static Acgc3dsGpuVertex* g_vertex_buffer;
static size_t g_frame_vertex_count;
static C3D_Tex* g_bound_texture;
static C3D_Tex* g_bound_texture1;

static int g_tev_state_valid;
static Acgc3dsTevPreset g_tev_preset;
static u32 g_tev_color1;
static u32 g_tev_color2;
static int g_blend_state_valid;
static int g_blend_mode;
static int g_blend_src;
static int g_blend_dst;
static int g_alpha_state_valid;
static int g_alpha_test;
static int g_alpha_func;
static int g_alpha_ref;
static int g_depth_state_valid;
static int g_depth_test;
static int g_depth_func;
static int g_depth_write;
static int g_color_update;
static int g_alpha_update;

/* Keep the off-screen target at its native logical size. Sub-native rendering
 * needs a different texture-origin/presentation transform; sampling the
 * upper portion of a partially used target can present only untouched pixels. */
static int g_render_scale = 100;
static int g_pending_render_scale = 100;
static int g_scene_width = ACGC_3DS_WORLD_W;
static int g_scene_height = ACGC_3DS_WORLD_H;
static int g_active_layer = -1;
static int g_viewport_valid;
static u32 g_viewport_x;
static u32 g_viewport_y;
static u32 g_viewport_w;
static u32 g_viewport_h;

static GPU_TESTFUNC acgc_3ds_video_depth_func(int gx_func) {
    /* Citro3D uses a reversed depth buffer (clear=0, near values are larger),
     * so reverse the ordered GX comparisons. GXCompare values are 0..7. */
    static const GPU_TESTFUNC funcs[8] = {
        GPU_NEVER, GPU_GREATER, GPU_EQUAL, GPU_GEQUAL,
        GPU_LESS, GPU_NOTEQUAL, GPU_LEQUAL, GPU_ALWAYS
    };
    return (unsigned)gx_func < 8 ? funcs[gx_func] : GPU_ALWAYS;
}

static GPU_TESTFUNC acgc_3ds_video_alpha_func(int gx_func) {
    static const GPU_TESTFUNC funcs[8] = {
        GPU_NEVER, GPU_LESS, GPU_EQUAL, GPU_LEQUAL,
        GPU_GREATER, GPU_NOTEQUAL, GPU_GEQUAL, GPU_ALWAYS
    };
    return (unsigned)gx_func < 8 ? funcs[gx_func] : GPU_ALWAYS;
}

static GPU_BLENDFACTOR acgc_3ds_video_blend_src(int gx_factor) {
    static const GPU_BLENDFACTOR factors[8] = {
        GPU_ZERO, GPU_ONE, GPU_DST_COLOR, GPU_ONE_MINUS_DST_COLOR,
        GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
        GPU_DST_ALPHA, GPU_ONE_MINUS_DST_ALPHA
    };
    return (unsigned)gx_factor < 8 ? factors[gx_factor] : GPU_ONE;
}

static GPU_BLENDFACTOR acgc_3ds_video_blend_dst(int gx_factor) {
    static const GPU_BLENDFACTOR factors[8] = {
        GPU_ZERO, GPU_ONE, GPU_SRC_COLOR, GPU_ONE_MINUS_SRC_COLOR,
        GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
        GPU_DST_ALPHA, GPU_ONE_MINUS_DST_ALPHA
    };
    return (unsigned)gx_factor < 8 ? factors[gx_factor] : GPU_ZERO;
}

static void acgc_3ds_video_delete_targets(void) {
    if (g_scene != NULL) {
        C3D_RenderTargetDelete(g_scene);
        g_scene = NULL;
    }
    if (g_ui != NULL) {
        C3D_RenderTargetDelete(g_ui);
        g_ui = NULL;
    }
    if (g_top != NULL) {
        C3D_RenderTargetDelete(g_top);
        g_top = NULL;
    }
    if (g_bottom != NULL) {
        C3D_RenderTargetDelete(g_bottom);
        g_bottom = NULL;
    }
    if (g_scene_texture_ready) {
        C3D_TexDelete(&g_scene_texture);
        g_scene_texture_ready = 0;
    }
    if (g_ui_texture_ready) {
        C3D_TexDelete(&g_ui_texture);
        g_ui_texture_ready = 0;
    }
}

static void acgc_3ds_video_bind_buffer(void) {
    C3D_BufInfo* buffer = C3D_GetBufInfo();
    BufInfo_Init(buffer);
    BufInfo_Add(buffer, g_vertex_buffer, sizeof(*g_vertex_buffer), 4, 0x3210);
}

static void acgc_3ds_video_apply_viewport(Acgc3dsRenderLayer layer,
                                           const float viewport[6]) {
    int canvas_w = layer == ACGC_3DS_RENDER_UI ? ACGC_3DS_UI_W : g_scene_width;
    int canvas_h = layer == ACGC_3DS_RENDER_UI ? ACGC_3DS_UI_H : g_scene_height;
    int x = 0;
    int y = 0;
    int w = canvas_w;
    int h = canvas_h;

    if (viewport != NULL && viewport[2] > 0.0f && viewport[3] > 0.0f) {
        float left = viewport[0] * (float)canvas_w / 640.0f;
        float top = viewport[1] * (float)canvas_h / 480.0f;
        float right = (viewport[0] + viewport[2]) * (float)canvas_w / 640.0f;
        float bottom = (viewport[1] + viewport[3]) * (float)canvas_h / 480.0f;

        x = (int)floorf(left + 0.5f);
        w = (int)floorf(right + 0.5f) - x;
        h = (int)floorf(bottom + 0.5f) - (int)floorf(top + 0.5f);
        y = canvas_h - (int)floorf(bottom + 0.5f);

        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > canvas_w) w = canvas_w - x;
        if (y + h > canvas_h) h = canvas_h - y;
        if (w <= 0 || h <= 0) { x = 0; y = 0; w = canvas_w; h = canvas_h; }
    }

    if (!g_viewport_valid || g_viewport_x != (u32)x || g_viewport_y != (u32)y ||
        g_viewport_w != (u32)w || g_viewport_h != (u32)h) {
        C3D_SetViewport((u32)x, (u32)y, (u32)w, (u32)h);
        g_viewport_x = (u32)x;
        g_viewport_y = (u32)y;
        g_viewport_w = (u32)w;
        g_viewport_h = (u32)h;
        g_viewport_valid = 1;
    }
}

static void acgc_3ds_video_select_layer(Acgc3dsRenderLayer layer,
                                         const float viewport[6]) {
    C3D_RenderTarget* target = layer == ACGC_3DS_RENDER_UI ? g_ui : g_scene;

    if (g_active_layer != (int)layer) {
        C3D_FrameDrawOn(target);
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        g_active_layer = (int)layer;
        g_viewport_valid = 0;
    }
    acgc_3ds_video_apply_viewport(layer, viewport);
}

int acgc_3ds_video_init(void) {
    gfxInitDefault();
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        gfxExit();
        return 0;
    }

    g_top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, -1);
    g_bottom = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, -1);
    if (g_top == NULL || g_bottom == NULL) goto fail;
    C3D_RenderTargetSetOutput(g_top, GFX_TOP, GFX_LEFT, ACGC_DISPLAY_TRANSFER_FLAGS);

    memset(&g_scene_texture, 0, sizeof(g_scene_texture));
    if (!C3D_TexInitVRAM(&g_scene_texture, ACGC_3DS_SCENE_TEX_W,
                         ACGC_3DS_SCENE_TEX_H, GPU_RGBA8)) goto fail;
    g_scene_texture_ready = 1;
    C3D_TexSetFilter(&g_scene_texture, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&g_scene_texture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    g_scene = C3D_RenderTargetCreateFromTex(&g_scene_texture, GPU_TEXFACE_2D, 0,
                                            GPU_RB_DEPTH24);
    if (g_scene == NULL) goto fail;

    memset(&g_ui_texture, 0, sizeof(g_ui_texture));
    if (!C3D_TexInitVRAM(&g_ui_texture, ACGC_3DS_SCENE_TEX_W,
                         ACGC_3DS_SCENE_TEX_H, GPU_RGBA8)) goto fail;
    g_ui_texture_ready = 1;
    C3D_TexSetFilter(&g_ui_texture, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&g_ui_texture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    g_ui = C3D_RenderTargetCreateFromTex(&g_ui_texture, GPU_TEXFACE_2D, 0, -1);
    if (g_ui == NULL) goto fail;

    g_shader_dvlb = DVLB_ParseFile((u32*)acgc_3ds_vshader_shbin,
                                   acgc_3ds_vshader_shbin_size);
    if (g_shader_dvlb == NULL) goto fail;
    shaderProgramInit(&g_shader_program);
    g_shader_program_ready = 1;
    shaderProgramSetVsh(&g_shader_program, &g_shader_dvlb->DVLE[0]);
    C3D_BindProgram(&g_shader_program);
    g_projection_uniform = shaderInstanceGetUniformLocation(
        g_shader_program.vertexShader, "projection");
    g_modelview_uniform = shaderInstanceGetUniformLocation(
        g_shader_program.vertexShader, "modelview");

    {
        C3D_AttrInfo* attr = C3D_GetAttrInfo();
        AttrInfo_Init(attr);
        AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);
        AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
        AttrInfo_AddLoader(attr, 2, GPU_FLOAT, 2);
        AttrInfo_AddLoader(attr, 3, GPU_FLOAT, 4);
    }

    g_vertex_buffer = linearAlloc(sizeof(*g_vertex_buffer) * ACGC_3DS_MAX_FRAME_VERTICES);
    if (g_vertex_buffer == NULL) goto fail;
    acgc_3ds_video_bind_buffer();

    for (int i = 0; i < 6; ++i) C3D_TexEnvInit(C3D_GetTexEnv(i));
    C3D_TexBind(0, NULL);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
    Mtx_Ortho(&g_projection, 0.0f, 400.0f, 0.0f, 240.0f, 0.0f, 1.0f, true);
    Mtx_Identity(&g_modelview);
    g_video_ready = 1;
    return 1;

fail:
    if (g_vertex_buffer != NULL) {
        linearFree(g_vertex_buffer);
        g_vertex_buffer = NULL;
    }
    if (g_shader_program_ready) {
        shaderProgramFree(&g_shader_program);
        g_shader_program_ready = 0;
    }
    if (g_shader_dvlb != NULL) {
        DVLB_Free(g_shader_dvlb);
        g_shader_dvlb = NULL;
    }
    acgc_3ds_video_delete_targets();
    C3D_Fini();
    gfxExit();
    return 0;
}

int acgc_3ds_video_ready(void) {
    return g_video_ready;
}

void acgc_3ds_video_enable_game_screens(void) {
    if (!g_video_ready || g_bottom_enabled) return;

    /* consoleInit switches the bottom screen to RGB565/single-buffered. The
     * native UI composite needs the normal RGB8 double-buffered framebuffer. */
    gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);
    C3D_RenderTargetSetOutput(g_bottom, GFX_BOTTOM, GFX_LEFT,
                              ACGC_DISPLAY_TRANSFER_FLAGS);
    g_bottom_enabled = 1;
}

int acgc_3ds_video_get_render_scale(void) {
    return g_render_scale;
}

void acgc_3ds_video_set_render_scale(int percent) {
    if (percent <= 62) g_pending_render_scale = 50;
    else if (percent <= 87) g_pending_render_scale = 75;
    else g_pending_render_scale = 100;
}

void acgc_3ds_video_begin_frame(void) {
    if (!g_video_ready || g_frame_active) return;

    /* Scale changes are committed only between FrameEnd and FrameBegin. The
     * texture-backed target is fixed-size, so changing resolution never frees
     * a resource that Citro3D may still reference. */
    g_render_scale = g_pending_render_scale;
    g_scene_width = ACGC_3DS_WORLD_W * g_render_scale / 100;
    g_scene_height = ACGC_3DS_WORLD_H * g_render_scale / 100;

    if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) return;
    g_frame_active = 1;
    g_frame_vertex_count = 0;
    C3D_RenderTargetClear(g_top, C3D_CLEAR_COLOR, 0x102030FF, 0);
    C3D_RenderTargetClear(g_scene, C3D_CLEAR_ALL, 0x102030FF, 0);
    C3D_RenderTargetClear(g_ui, C3D_CLEAR_COLOR, 0x00000000, 0);
    g_active_layer = -1;
    g_viewport_valid = 0;
    acgc_3ds_video_select_layer(ACGC_3DS_RENDER_WORLD, NULL);
    C3D_BindProgram(&g_shader_program);
    acgc_3ds_video_bind_buffer();
    g_sent_projection_valid = 0;
    g_sent_modelview_valid = 0;
    g_bound_texture = NULL;
    g_bound_texture1 = NULL;
    g_tev_state_valid = 0;
    g_blend_state_valid = 0;
    g_alpha_state_valid = 0;
    g_depth_state_valid = 0;
}

static void acgc_3ds_video_copy_mtx(C3D_Mtx* out, const float in[4][4]) {
    out->r[0].x = in[0][0]; out->r[0].y = in[0][1]; out->r[0].z = in[0][2]; out->r[0].w = in[0][3];
    out->r[1].x = in[1][0]; out->r[1].y = in[1][1]; out->r[1].z = in[1][2]; out->r[1].w = in[1][3];
    out->r[2].x = in[2][0]; out->r[2].y = in[2][1]; out->r[2].z = in[2][2]; out->r[2].w = in[2][3];
    out->r[3].x = in[3][0]; out->r[3].y = in[3][1]; out->r[3].z = in[3][2]; out->r[3].w = in[3][3];
}

int acgc_3ds_video_draw_triangles(const Acgc3dsGpuVertex* vertices, size_t count) {
    if (!g_video_ready || !g_frame_active) return 0;
    acgc_3ds_video_select_layer(ACGC_3DS_RENDER_WORLD, NULL);
    Mtx_Ortho(&g_projection, 0.0f, 400.0f, 0.0f, 240.0f, 0.0f, 1.0f, true);
    Mtx_Identity(&g_modelview);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_projection_uniform, &g_projection);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_modelview_uniform, &g_modelview);
    memcpy(&g_sent_projection, &g_projection, sizeof(g_projection));
    memcpy(&g_sent_modelview, &g_modelview, sizeof(g_modelview));
    g_sent_projection_valid = 1;
    g_sent_modelview_valid = 1;
    return acgc_3ds_video_draw_gx_triangles(vertices, count, NULL, NULL,
                                             NULL, NULL, 0xffffffffu, 0xffffffffu,
                                             ACGC_3DS_TEV_PASSCLR,
                                             ACGC_3DS_RENDER_WORLD, NULL,
                                             0, 1, 0,
                                             0, 7, 0,
                                             0, 7, 0, 1, 1);
}

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
                                     int blend_mode,
                                     int blend_src,
                                     int blend_dst,
                                     int alpha_test,
                                     int alpha_func,
                                     int alpha_ref,
                                     int depth_test,
                                     int depth_func,
                                     int depth_write,
                                     int color_update,
                                     int alpha_update) {
    Acgc3dsGpuVertex* dst;
    size_t present_reserve = g_bottom_enabled ? 12u : 6u;

    if (!g_video_ready || !g_frame_active || vertices == NULL || count == 0 ||
        count > ACGC_3DS_MAX_FRAME_VERTICES - present_reserve || count % 3 != 0) return 0;
    if (g_frame_vertex_count + count > ACGC_3DS_MAX_FRAME_VERTICES - present_reserve) return 0;

    acgc_3ds_video_select_layer(layer, viewport);
    if (!g_blend_state_valid || g_blend_mode != blend_mode ||
        g_blend_src != blend_src || g_blend_dst != blend_dst) {
        GPU_BLENDEQUATION equation = blend_mode == 3 ?
                                     GPU_BLEND_REVERSE_SUBTRACT : GPU_BLEND_ADD;
        GPU_BLENDFACTOR src = blend_mode == 1 ?
                              acgc_3ds_video_blend_src(blend_src) : GPU_ONE;
        GPU_BLENDFACTOR dst = blend_mode == 1 ?
                              acgc_3ds_video_blend_dst(blend_dst) : GPU_ZERO;
        if (blend_mode == 3) {
            src = GPU_ONE;
            dst = GPU_ONE;
        }
        C3D_AlphaBlend(equation, equation, src, dst, src, dst);
        g_blend_mode = blend_mode;
        g_blend_src = blend_src;
        g_blend_dst = blend_dst;
        g_blend_state_valid = 1;
    }
    if (!g_alpha_state_valid || g_alpha_test != alpha_test ||
        g_alpha_func != alpha_func || g_alpha_ref != alpha_ref) {
        C3D_AlphaTest(alpha_test,
                      alpha_test ? acgc_3ds_video_alpha_func(alpha_func) : GPU_ALWAYS,
                      (u8)alpha_ref);
        g_alpha_test = alpha_test;
        g_alpha_func = alpha_func;
        g_alpha_ref = alpha_ref;
        g_alpha_state_valid = 1;
    }
    if (!g_depth_state_valid || g_depth_test != depth_test ||
        g_depth_func != depth_func || g_depth_write != depth_write ||
        g_color_update != color_update || g_alpha_update != alpha_update) {
        GPU_WRITEMASK write_mask = 0;
        if (color_update) write_mask |= GPU_WRITE_RED | GPU_WRITE_GREEN | GPU_WRITE_BLUE;
        if (alpha_update) write_mask |= GPU_WRITE_ALPHA;
        if (depth_write) write_mask |= GPU_WRITE_DEPTH;
        C3D_DepthTest(depth_test || depth_write,
                      depth_test ? acgc_3ds_video_depth_func(depth_func) : GPU_ALWAYS,
                      write_mask);
        g_depth_test = depth_test;
        g_depth_func = depth_func;
        g_depth_write = depth_write;
        g_color_update = color_update;
        g_alpha_update = alpha_update;
        g_depth_state_valid = 1;
    }
    if (projection != NULL) {
        acgc_3ds_video_copy_mtx(&g_projection, projection);
        if (!g_sent_projection_valid ||
            memcmp(&g_sent_projection, &g_projection, sizeof(g_projection)) != 0) {
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_projection_uniform, &g_projection);
            memcpy(&g_sent_projection, &g_projection, sizeof(g_projection));
            g_sent_projection_valid = 1;
        }
    }
    if (modelview != NULL) {
        acgc_3ds_video_copy_mtx(&g_modelview, modelview);
        if (!g_sent_modelview_valid ||
            memcmp(&g_sent_modelview, &g_modelview, sizeof(g_modelview)) != 0) {
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_modelview_uniform, &g_modelview);
            memcpy(&g_sent_modelview, &g_modelview, sizeof(g_modelview));
            g_sent_modelview_valid = 1;
        }
    }

    if (!g_tev_state_valid || g_bound_texture != (C3D_Tex*)texture0 ||
        g_bound_texture1 != (C3D_Tex*)texture1 || g_tev_preset != tev_preset ||
        g_tev_color1 != tev_color1 || g_tev_color2 != tev_color2) {
        C3D_TexEnv* env = C3D_GetTexEnv(0);
        C3D_Tex* next_texture = (C3D_Tex*)texture0;

        for (int i = 0; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
        /* citro3d permits NULL for texture unit 0, but C3D_TexBind dereferences
         * the texture unconditionally for units 1/2 on real hardware. A stale
         * unit-1 binding is harmless when the reinitialized TEV stages do not
         * reference GPU_TEXTURE1. */
        if (texture1 != NULL) C3D_TexBind(1, (C3D_Tex*)texture1);
        if (next_texture == NULL || tev_preset == ACGC_3DS_TEV_PASSCLR) {
            C3D_TexBind(0, NULL);
            C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        } else {
            C3D_TexBind(0, next_texture);
            switch (tev_preset) {
                case ACGC_3DS_TEV_SPOTLIGHT: {
                    C3D_TexEnv* env1 = C3D_GetTexEnv(1);
                    C3D_TexEnv* env2 = C3D_GetTexEnv(2);

                    /* Scene 19 uses TEX0 for the spotlight gradient and TEX1
                     * as its opacity mask. Reproduce the original two-texture
                     * GX equation instead of treating TEX0 as an opaque image. */
                    C3D_TexEnvColor(env, tev_color2);
                    C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, 0, 0);
                    C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
                    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, 0, 0);
                    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);

                    C3D_TexEnvColor(env1, tev_color1);
                    C3D_TexEnvSrc(env1, C3D_RGB,
                                  GPU_CONSTANT, GPU_PREVIOUS, GPU_TEXTURE0);
                    C3D_TexEnvFunc(env1, C3D_RGB, GPU_INTERPOLATE);
                    C3D_TexEnvSrc(env1, C3D_Alpha, GPU_PREVIOUS, 0, 0);
                    C3D_TexEnvFunc(env1, C3D_Alpha, GPU_REPLACE);

                    C3D_TexEnvSrc(env2, C3D_RGB, GPU_PREVIOUS, 0, 0);
                    C3D_TexEnvFunc(env2, C3D_RGB, GPU_REPLACE);
                    C3D_TexEnvSrc(env2, C3D_Alpha,
                                  GPU_TEXTURE1, GPU_PREVIOUS, GPU_TEXTURE1);
                    C3D_TexEnvFunc(env2, C3D_Alpha, GPU_MULTIPLY_ADD);
                    break;
                }
                case ACGC_3DS_TEV_MASKED:
                    /* TEX0 supplies color while TEX1 is a distinct opacity
                     * mask (used by UI shapes such as the inventory oval). */
                    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, 0, 0);
                    C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
                    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE1, 0, 0);
                    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
                    break;
                case ACGC_3DS_TEV_FONT_MASK:
                    /* Font I4 stores coverage in texture alpha. RGB and the
                     * remaining opacity both come from GX primitive color. */
                    C3D_TexEnvColor(env, tev_color1);
                    C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, 0, 0);
                    C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
                    C3D_TexEnvSrc(env, C3D_Alpha,
                                  GPU_TEXTURE0, GPU_CONSTANT, 0);
                    C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
                    break;
                case ACGC_3DS_TEV_HUD_TINT:
                    /* HUD mask textures provide shape/coverage only. Their
                     * visible color is the N64 primitive color, not the
                     * normally-white vertex raster color. */
                    C3D_TexEnvColor(env, tev_color1);
                    C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, 0, 0);
                    C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
                    C3D_TexEnvSrc(env, C3D_Alpha,
                                  GPU_TEXTURE0, GPU_CONSTANT, 0);
                    C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
                    break;
                case ACGC_3DS_TEV_HUD_GRADIENT: {
                    C3D_TexEnv* env1 = C3D_GetTexEnv(1);

                    /* (primitive - environment) * intensity + environment.
                     * This is the clock/window combiner used throughout the
                     * HUD. A second stage is needed because PICA exposes one
                     * constant color per TEV stage. */
                    C3D_TexEnvColor(env, tev_color2);
                    C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, 0, 0);
                    C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
                    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, 0, 0);
                    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);

                    C3D_TexEnvColor(env1, tev_color1);
                    C3D_TexEnvSrc(env1, C3D_RGB,
                                  GPU_CONSTANT, GPU_PREVIOUS, GPU_TEXTURE0);
                    C3D_TexEnvFunc(env1, C3D_RGB, GPU_INTERPOLATE);
                    C3D_TexEnvSrc(env1, C3D_Alpha,
                                  GPU_PREVIOUS, GPU_CONSTANT, 0);
                    C3D_TexEnvFunc(env1, C3D_Alpha, GPU_MODULATE);
                    break;
                }
                case ACGC_3DS_TEV_REPLACE:
                    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
                    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
                    break;
                case ACGC_3DS_TEV_DECAL:
                    C3D_TexEnvSrc(env, C3D_RGB,
                                  GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_TEXTURE0);
                    C3D_TexEnvOpRgb(env,
                                    GPU_TEVOP_RGB_SRC_COLOR,
                                    GPU_TEVOP_RGB_SRC_COLOR,
                                    GPU_TEVOP_RGB_SRC_ALPHA);
                    C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
                    C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, 0, 0);
                    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
                    break;
                case ACGC_3DS_TEV_BLEND:
                    C3D_TexEnvColor(env, 0xffffffffu);
                    C3D_TexEnvSrc(env, C3D_RGB,
                                  GPU_PRIMARY_COLOR, GPU_CONSTANT, GPU_TEXTURE0);
                    C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
                    C3D_TexEnvSrc(env, C3D_Alpha,
                                  GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
                    C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
                    break;
                case ACGC_3DS_TEV_MODULATE:
                case ACGC_3DS_TEV_FALLBACK:
                default:
                    C3D_TexEnvSrc(env, C3D_Both,
                                  GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
                    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
                    break;
            }
        }
        g_bound_texture = next_texture;
        g_bound_texture1 = (C3D_Tex*)texture1;
        g_tev_preset = tev_preset;
        g_tev_color1 = tev_color1;
        g_tev_color2 = tev_color2;
        g_tev_state_valid = 1;
    }

    dst = &g_vertex_buffer[g_frame_vertex_count];
    memcpy(dst, vertices, count * sizeof(*vertices));
    /* The GPU (and Azahar's command processor) may consume the vertex range
     * as soon as DrawArrays is recorded. Make CPU writes visible first. */
    GSPGPU_FlushDataCache(dst, count * sizeof(*dst));
    C3D_DrawArrays(GPU_TRIANGLES, (int)g_frame_vertex_count, (int)count);
    g_frame_vertex_count += count;
    return 1;
}

static void acgc_3ds_video_present(C3D_RenderTarget* target, C3D_Tex* texture,
                                   int source_w, int source_h,
                                   int logical_w, int logical_h) {
    Acgc3dsGpuVertex* v;
    float u0 = 0.0f;
    float u1 = (float)source_w / (float)ACGC_3DS_SCENE_TEX_W;
    float v0 = 1.0f;
    float v1 = 1.0f - (float)source_h / (float)ACGC_3DS_SCENE_TEX_H;
    C3D_TexEnv* env;

    /* Mtx_OrthoTilt supplies the framebuffer's quarter-turn. Texture-backed
     * layers need their vertical texture axis inverted for both screens. */
    if (target == g_top || target == g_bottom) {
        float swap = v0;
        v0 = v1;
        v1 = swap;
    }

    if (g_frame_vertex_count + 6 > ACGC_3DS_MAX_FRAME_VERTICES) return;
    C3D_FrameDrawOn(target);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    Mtx_OrthoTilt(&g_projection, 0.0f, (float)logical_w,
                  0.0f, (float)logical_h, 0.0f, 1.0f, true);
    Mtx_Identity(&g_modelview);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_projection_uniform, &g_projection);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_modelview_uniform, &g_modelview);
    C3D_TexBind(0, texture);
    env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);

    v = &g_vertex_buffer[g_frame_vertex_count];
#define SET_PRESENT_VERTEX(dst, px, py, tu, tv) do { \
        (dst).x = (float)(px); (dst).y = (float)(py); (dst).z = 0.5f; \
        (dst).s = (tu); (dst).t = (tv); \
        (dst).s1 = (tu); (dst).t1 = (tv); \
        (dst).r = 1.0f; (dst).g = 1.0f; (dst).b = 1.0f; (dst).a = 1.0f; \
    } while (0)
    SET_PRESENT_VERTEX(v[0], 0,         0,         u0, v0);
    SET_PRESENT_VERTEX(v[1], 0,         logical_h, u0, v1);
    SET_PRESENT_VERTEX(v[2], logical_w, logical_h, u1, v1);
    SET_PRESENT_VERTEX(v[3], 0,         0,         u0, v0);
    SET_PRESENT_VERTEX(v[4], logical_w, logical_h, u1, v1);
    SET_PRESENT_VERTEX(v[5], logical_w, 0,         u1, v0);
#undef SET_PRESENT_VERTEX

    GSPGPU_FlushDataCache(v, 6 * sizeof(*v));
    C3D_DrawArrays(GPU_TRIANGLES, (int)g_frame_vertex_count, 6);
    g_frame_vertex_count += 6;
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
    g_sent_projection_valid = 0;
    g_sent_modelview_valid = 0;
    g_bound_texture = texture;
    g_bound_texture1 = NULL;
    g_tev_state_valid = 0;
    g_blend_state_valid = 0;
    g_alpha_state_valid = 0;
    g_depth_state_valid = 0;
}

void acgc_3ds_video_end_frame(void) {
    if (!g_video_ready || !g_frame_active) return;

    /* Both render layers are texture-backed. Presenting the top layer through
     * Mtx_OrthoTilt applies the 3DS framebuffer's required 90-degree clockwise
     * rotation instead of relying on the world projection to happen to match
     * the portrait-oriented render target. */
    C3D_FrameSplit(0);
    C3D_RenderTargetClear(g_top, C3D_CLEAR_COLOR, 0x102030FF, 0);
    acgc_3ds_video_present(g_top, &g_scene_texture,
                           g_scene_width, g_scene_height,
                           ACGC_3DS_WORLD_W, ACGC_3DS_WORLD_H);
    if (g_bottom_enabled) {
        C3D_RenderTargetClear(g_bottom, C3D_CLEAR_COLOR, 0x000000FF, 0);
        acgc_3ds_video_present(g_bottom, &g_ui_texture,
                               ACGC_3DS_UI_W, ACGC_3DS_UI_H,
                               ACGC_3DS_UI_W, ACGC_3DS_UI_H);
    }
    C3D_FrameEnd(0);
    g_frame_active = 0;
    g_active_layer = -1;
}

void acgc_3ds_video_shutdown(void) {
    if (!g_video_ready) return;

    if (g_frame_active) {
        C3D_FrameEnd(0);
        g_frame_active = 0;
    }

    /* Target deletion waits for/clears the queued GPU work. Keep buffers and
     * shader resources valid until that synchronization point has passed. */
    acgc_3ds_video_delete_targets();
    linearFree(g_vertex_buffer);
    g_vertex_buffer = NULL;
    if (g_shader_program_ready) {
        shaderProgramFree(&g_shader_program);
        g_shader_program_ready = 0;
    }
    if (g_shader_dvlb != NULL) {
        DVLB_Free(g_shader_dvlb);
        g_shader_dvlb = NULL;
    }
    C3D_Fini();
    gfxExit();
    g_bottom_enabled = 0;
    g_video_ready = 0;
}
