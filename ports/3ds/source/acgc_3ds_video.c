#include "acgc_3ds_platform.h"

#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <string.h>

#include "acgc_3ds_vshader_shbin.h"

#define ACGC_3DS_MAX_FRAME_VERTICES 32768u
#define ACGC_3DS_PRESENT_VERTICES 12u
#define ACGC_3DS_DASHBOARD_VERTICES 2048u
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
    int canvas_w = layer == ACGC_3DS_RENDER_UI ? ACGC_3DS_UI_W : ACGC_3DS_WORLD_W;
    int canvas_h = layer == ACGC_3DS_RENDER_UI ? ACGC_3DS_UI_H : ACGC_3DS_WORLD_H;
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
    /* Both layers use fixed logical dimensions so their viewports remain
     * aligned when the UI is composited over the world. */
    C3D_RenderTarget* target = layer == ACGC_3DS_RENDER_UI ? g_ui : g_scene;

    if (g_active_layer != (int)layer) {
        C3D_FrameDrawOn(target);
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        g_active_layer = (int)layer;
        g_viewport_valid = 0;
        /* UI draws accumulate into a transparent, premultiplied target while
         * world draws retain the native GX alpha equation. Force the cached
         * blend state to be rebuilt whenever those policies switch. */
        g_blend_state_valid = 0;
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

void acgc_3ds_video_begin_frame(void) {
    if (!g_video_ready || g_frame_active) return;

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
    /* End-frame always submits two top-screen quads. The dashboard adds a
     * fixed set of shapes and bitmap glyph cells; reserve a rounded-up budget
     * so a geometry-heavy world frame cannot truncate the bottom screen. */
    size_t present_reserve = ACGC_3DS_PRESENT_VERTICES +
        (g_bottom_enabled ? ACGC_3DS_DASHBOARD_VERTICES : 0u);

    if (!g_video_ready || !g_frame_active || vertices == NULL || count == 0 ||
        count > ACGC_3DS_MAX_FRAME_VERTICES - present_reserve || count % 3 != 0) return 0;
    if (g_frame_vertex_count + count > ACGC_3DS_MAX_FRAME_VERTICES - present_reserve) return 0;

    acgc_3ds_video_select_layer(layer, viewport);
    if (!g_blend_state_valid || g_blend_mode != blend_mode ||
        g_blend_src != blend_src || g_blend_dst != blend_dst) {
        GPU_BLENDEQUATION color_equation = blend_mode == 3 ?
                                           GPU_BLEND_REVERSE_SUBTRACT : GPU_BLEND_ADD;
        GPU_BLENDEQUATION alpha_equation = color_equation;
        GPU_BLENDFACTOR color_src = blend_mode == 1 ?
                                    acgc_3ds_video_blend_src(blend_src) : GPU_ONE;
        GPU_BLENDFACTOR color_dst = blend_mode == 1 ?
                                    acgc_3ds_video_blend_dst(blend_dst) : GPU_ZERO;
        GPU_BLENDFACTOR alpha_src = color_src;
        GPU_BLENDFACTOR alpha_dst = color_dst;
        if (blend_mode == 3) {
            color_src = GPU_ONE;
            color_dst = GPU_ONE;
            alpha_src = GPU_ONE;
            alpha_dst = GPU_ONE;
        }
        if (layer == ACGC_3DS_RENDER_UI) {
            /* RGB is already multiplied by the GX color blend. Track coverage
             * separately so presenting the intermediate texture does not
             * multiply translucent windows, fades, or glyph edges twice. */
            alpha_equation = GPU_BLEND_ADD;
            alpha_src = GPU_ONE;
            alpha_dst = (blend_mode == 1 || blend_mode == 3) ?
                        GPU_ONE_MINUS_SRC_ALPHA : GPU_ZERO;
        }
        C3D_AlphaBlend(color_equation, alpha_equation,
                       color_src, color_dst, alpha_src, alpha_dst);
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

static void acgc_3ds_video_present_region(C3D_RenderTarget* target, C3D_Tex* texture,
                                          int source_w, int source_h,
                                          int logical_w, int logical_h,
                                          int x, int y, int w, int h,
                                          int alpha_blend) {
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
    /* Presentation is a fresh fullscreen pass. GX draws may have left later
     * TEV stages active, or disabled individual color/depth writes; none of
     * that state is meaningful when copying the completed layer texture. */
    for (int i = 0; i < 6; ++i) C3D_TexEnvInit(C3D_GetTexEnv(i));
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    if (alpha_blend) {
        /* The UI texture stores premultiplied RGB and accumulated coverage. */
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                       GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA,
                       GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
    } else {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                       GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    }
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);

    v = &g_vertex_buffer[g_frame_vertex_count];
#define SET_PRESENT_VERTEX(dst, px, py, tu, tv) do { \
        (dst).x = (float)(px); (dst).y = (float)(py); (dst).z = 0.5f; \
        (dst).s = (tu); (dst).t = (tv); \
        (dst).s1 = (tu); (dst).t1 = (tv); \
        (dst).r = 1.0f; (dst).g = 1.0f; (dst).b = 1.0f; (dst).a = 1.0f; \
    } while (0)
    SET_PRESENT_VERTEX(v[0], x,     y,     u0, v0);
    SET_PRESENT_VERTEX(v[1], x,     y + h, u0, v1);
    SET_PRESENT_VERTEX(v[2], x + w, y + h, u1, v1);
    SET_PRESENT_VERTEX(v[3], x,     y,     u0, v0);
    SET_PRESENT_VERTEX(v[4], x + w, y + h, u1, v1);
    SET_PRESENT_VERTEX(v[5], x + w, y,     u1, v0);
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

static void acgc_3ds_video_present(C3D_RenderTarget* target, C3D_Tex* texture,
                                   int source_w, int source_h,
                                   int logical_w, int logical_h) {
    acgc_3ds_video_present_region(target, texture, source_w, source_h,
                                  logical_w, logical_h, 0, 0,
                                  logical_w, logical_h, 0);
}

static void dashboard_vertex(Acgc3dsGpuVertex* v, float x, float y, u32 rgba) {
    v->x = x;
    v->y = y;
    v->z = 0.5f;
    v->s = v->t = v->s1 = v->t1 = 0.0f;
    v->r = ((rgba >> 24) & 0xff) / 255.0f;
    v->g = ((rgba >> 16) & 0xff) / 255.0f;
    v->b = ((rgba >> 8) & 0xff) / 255.0f;
    v->a = (rgba & 0xff) / 255.0f;
}

static void dashboard_quad(float x0, float y0, float x1, float y1, u32 rgba) {
    Acgc3dsGpuVertex* v;
    if (g_frame_vertex_count + 6 > ACGC_3DS_MAX_FRAME_VERTICES) return;
    v = &g_vertex_buffer[g_frame_vertex_count];
    dashboard_vertex(&v[0], x0, y0, rgba);
    dashboard_vertex(&v[1], x0, y1, rgba);
    dashboard_vertex(&v[2], x1, y1, rgba);
    dashboard_vertex(&v[3], x0, y0, rgba);
    dashboard_vertex(&v[4], x1, y1, rgba);
    dashboard_vertex(&v[5], x1, y0, rgba);
    g_frame_vertex_count += 6;
}

static unsigned dashboard_glyph(char c) {
    switch (c) {
        case 'A': return 0x2BED;
        case 'C': return 0x7927;
        case 'D': return 0x6B6E;
        case 'E': return 0x79A7;
        case 'I': return 0x7497;
        case 'K': return 0x5BAD;
        case 'L': return 0x4927;
        case 'M': return 0x5FED;
        case 'N': return 0x5FFD;
        case 'O': return 0x7B6F;
        case 'P': return 0x7BE4;
        case 'S': return 0x79CF;
        case 'T': return 0x7492;
        case 'W': return 0x5BFD;
        case 'Y': return 0x5A92;
        default: return 0;
    }
}

static void dashboard_text(const char* text, float x, float y, float scale, u32 rgba) {
    while (*text != '\0') {
        unsigned bits = dashboard_glyph(*text++);
        int row;
        int col;
        for (row = 0; row < 5; ++row) {
            for (col = 0; col < 3; ++col) {
                if (bits & (1u << (14 - (row * 3 + col)))) {
                    dashboard_quad(x + col * scale, y + row * scale,
                                   x + (col + 1) * scale, y + (row + 1) * scale,
                                   rgba);
                }
            }
        }
        x += 4.0f * scale;
    }
}

static void dashboard_card(float x0, float x1, u32 color, int active) {
    u32 edge = active ? 0xFFF3B8FF : 0x41644AFF;
    dashboard_quad(x0, 38, x1, 136, edge);
    dashboard_quad(x0 + 3, 41, x1 - 3, 133, color);
    if (active) dashboard_quad(x0 + 7, 45, x1 - 7, 49, 0xFFFFFF80);
}

static void acgc_3ds_video_draw_dashboard(void) {
    size_t first = g_frame_vertex_count;
    u32 action = acgc_3ds_input_touch_action();
    C3D_TexEnv* env;

    C3D_FrameDrawOn(g_bottom);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    /* 3DS LCD framebuffers are stored rotated: the 320x240 bottom screen is
     * a 240x320 render target. Using the logical dimensions as the viewport
     * clips 80 physical rows and scales the remainder. Keep the viewport in
     * framebuffer space, then let OrthoTilt map our 320x240 UI coordinates.
     * Reversing bottom/top gives the dashboard and touch input the same
     * conventional top-left origin, instead of vertically mirroring the UI. */
    C3D_SetViewport(0, 0, ACGC_3DS_UI_H, ACGC_3DS_UI_W);
    Mtx_OrthoTilt(&g_projection, 0.0f, 320.0f, 240.0f, 0.0f,
                  0.0f, 1.0f, true);
    Mtx_Identity(&g_modelview);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_projection_uniform, &g_projection);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_modelview_uniform, &g_modelview);
    C3D_TexBind(0, NULL);
    env = C3D_GetTexEnv(0);
    for (int i = 0; i < 6; ++i) C3D_TexEnvInit(C3D_GetTexEnv(i));
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);

    /* A quiet, paper-and-felt control deck inspired by the game's stationery,
     * rather than a second copy of the GameCube HUD. */
    dashboard_quad(0, 0, 320, 240, 0xDDE8C8FF);
    dashboard_quad(0, 0, 320, 30, 0x41644AFF);
    dashboard_quad(0, 30, 320, 34, 0xE6B566FF);
    dashboard_text("TOWN DECK", 105, 10, 2.0f, 0xFFF8DDFF);
    dashboard_quad(17, 18, 25, 26, 0x9DC08BFF);
    dashboard_quad(295, 7, 303, 15, 0x9DC08BFF);

    dashboard_card(12, 104, 0x75A78DFF, action == ACGC_3DS_TOUCH_MAP);
    dashboard_card(114, 206, 0xD79062FF, action == ACGC_3DS_TOUCH_POCKETS);
    dashboard_card(216, 308, 0x7F91B7FF, action == ACGC_3DS_TOUCH_OPTIONS);

    /* Map folds. */
    dashboard_quad(30, 59, 48, 100, 0xFFF4D6FF);
    dashboard_quad(50, 54, 68, 95, 0xF6E4B5FF);
    dashboard_quad(70, 59, 88, 100, 0xFFF4D6FF);
    dashboard_quad(48, 58, 50, 96, 0x41644AFF);
    dashboard_quad(68, 58, 70, 96, 0x41644AFF);
    dashboard_text("MAP", 39, 112, 2.0f, 0x173D32FF);

    /* Pocket bag and clasp. */
    dashboard_quad(137, 65, 183, 101, 0xFFF0D0FF);
    dashboard_quad(143, 58, 177, 70, 0xFFF0D0FF);
    dashboard_quad(151, 53, 169, 58, 0xFFF0D0FF);
    dashboard_quad(158, 76, 163, 83, 0xA45F43FF);
    dashboard_text("POCKETS", 130, 112, 1.5f, 0x4B291FFF);

    /* Three friendly adjustment sliders. */
    dashboard_quad(237, 62, 287, 66, 0xEEF2FFFF);
    dashboard_quad(237, 79, 287, 83, 0xEEF2FFFF);
    dashboard_quad(237, 96, 287, 100, 0xEEF2FFFF);
    dashboard_quad(247, 57, 254, 71, 0xD7E0FFFF);
    dashboard_quad(270, 74, 277, 88, 0xD7E0FFFF);
    dashboard_quad(256, 91, 263, 105, 0xD7E0FFFF);
    dashboard_text("OPTIONS", 231, 112, 1.5f, 0x243153FF);

    /* Look pad: broad enough for a thumb, and useful on systems without a
     * C-Stick. The highlighted half follows the current touch direction. */
    dashboard_quad(12, 148, 308, 232, 0x41644AFF);
    dashboard_quad(15, 151, 305, 229, 0xEDF2DEFF);
    dashboard_quad(18, 154, 302, 190,
                   action == ACGC_3DS_TOUCH_LOOK_UP ? 0xCFE1B7FF : 0xE4ECD4FF);
    dashboard_quad(18, 192, 302, 226,
                   action == ACGC_3DS_TOUCH_LOOK_DOWN ? 0xCFE1B7FF : 0xE4ECD4FF);
    dashboard_quad(18, 154, 158, 226,
                   action == ACGC_3DS_TOUCH_LOOK_LEFT ? 0xCFE1B7A0 : 0xFFFFFF00);
    dashboard_quad(162, 154, 302, 226,
                   action == ACGC_3DS_TOUCH_LOOK_RIGHT ? 0xCFE1B7A0 : 0xFFFFFF00);
    dashboard_text("LOOK", 144, 183, 2.0f, 0x41644AFF);
    dashboard_quad(31, 187, 47, 191, 0x41644AFF);
    dashboard_quad(273, 187, 289, 191, 0x41644AFF);
    dashboard_quad(158, 160, 162, 174, 0x41644AFF);
    dashboard_quad(158, 207, 162, 221, 0x41644AFF);

    if (g_frame_vertex_count > first) {
        GSPGPU_FlushDataCache(&g_vertex_buffer[first],
                              (g_frame_vertex_count - first) * sizeof(*g_vertex_buffer));
        C3D_DrawArrays(GPU_TRIANGLES, (int)first,
                       (int)(g_frame_vertex_count - first));
    }
    g_sent_projection_valid = 0;
    g_sent_modelview_valid = 0;
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
                           ACGC_3DS_WORLD_W, ACGC_3DS_WORLD_H,
                           ACGC_3DS_WORLD_W, ACGC_3DS_WORLD_H);
    acgc_3ds_video_present_region(g_top, &g_ui_texture,
                                  ACGC_3DS_UI_W, ACGC_3DS_UI_H,
                                  ACGC_3DS_WORLD_W, ACGC_3DS_WORLD_H,
                                  40, 0, ACGC_3DS_UI_W, ACGC_3DS_UI_H, 1);
    if (g_bottom_enabled) {
        C3D_RenderTargetClear(g_bottom, C3D_CLEAR_COLOR, 0x000000FF, 0);
        acgc_3ds_video_draw_dashboard();
    }
    C3D_FrameEnd(0);
    g_frame_active = 0;
    g_active_layer = -1;
}

void acgc_3ds_video_shutdown(void) {
    extern void pc_gx_shutdown(void);

    if (!g_video_ready) return;

    if (g_frame_active) {
        C3D_FrameEnd(0);
        g_frame_active = 0;
    }

    /* Texture-cache entries may still be referenced by the last submitted
     * command buffer. Synchronize before deleting them, and do so while the
     * Citro3D allocator is still alive. */
    C3D_FrameSync();
    pc_gx_shutdown();

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
