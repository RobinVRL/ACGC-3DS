#include "acgc_3ds_platform.h"
#include "acgc_3ds_texture_pack.h"

#include <dolphin/gx/GXCommandList.h>
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ACGC_GX_MAX_INPUT 2048
#define ACGC_GX_MAX_OUTPUT 4096

typedef struct AcgcGxVertex {
    f32 x, y, z;
    f32 nx, ny, nz;
    u8 r, g, b, a;
    f32 s, t;
} AcgcGxVertex;

static AcgcGxVertex s_vertices[ACGC_GX_MAX_INPUT];
static Acgc3dsGpuVertex s_output[ACGC_GX_MAX_OUTPUT];
static AcgcGxVertex s_current;
static u16 s_count;
static u16 s_expected_count;
static GXPrimitive s_primitive;
static int s_pending;
static f32 s_projection[4][4];
static f32 s_position[10][3][4];
static f32 s_normal[10][3][3];
static f32 s_texture_mtx[10][3][4];
static u32 s_texgen_mtx[8];
static u32 s_current_mtx;
static f32 s_viewport[6] = { 0, 0, 640, 480, 0, 1 };
static int s_render_screen;
static GXDrawDoneCallback s_draw_done_callback;
static GXFifoObj s_fifo;
static GXAttrType s_vtx_desc[GX_VA_MAX_ATTR];
static GXCompCnt s_vtx_cnt[GX_MAX_VTXFMT][GX_VA_MAX_ATTR];
static GXCompType s_vtx_type[GX_MAX_VTXFMT][GX_VA_MAX_ATTR];
static u8 s_vtx_frac[GX_MAX_VTXFMT][GX_VA_MAX_ATTR];
static const u8* s_array_base[GX_VA_MAX_ATTR];
static u8 s_array_stride[GX_VA_MAX_ATTR];
static u8* s_dl_buffer;
static u32 s_dl_size;
static u32 s_dl_offset;
static int s_dl_active;
static int s_dl_overflow;
static C3D_Tex* s_bound_texture;
static u32 s_bound_texture_format;
static u16 s_bound_texture_width;
static u16 s_bound_texture_height;
static u16 s_bound_texture_upload_width;
static u16 s_bound_texture_upload_height;
static GXTexMapID s_active_texmap = GX_TEXMAP_NULL;
static GXTexCoordID s_active_texcoord = GX_TEXCOORD0;
static int s_active_tev_stage;
static GXColor s_tev_colors[4];

typedef struct Acgc3dsTevStage {
    GXTevColorArg color[4];
    GXTevAlphaArg alpha[4];
    GXTevOp color_op;
    GXTevOp alpha_op;
    GXTevBias color_bias;
    GXTevBias alpha_bias;
    GXTevScale color_scale;
    GXTevScale alpha_scale;
    GXBool color_clamp;
    GXBool alpha_clamp;
    GXTevRegID color_out;
    GXTevRegID alpha_out;
    GXTexCoordID texcoord;
    GXTexMapID texmap;
    GXChannelID channel;
} Acgc3dsTevStage;

static Acgc3dsTevStage s_tev_stages[GX_MAX_TEVSTAGE];
static u8 s_num_tev_stages = 1;

#define TEXOBJ_IMAGE_PTR   0
#define TEXOBJ_WIDTH       1
#define TEXOBJ_HEIGHT      2
#define TEXOBJ_FORMAT      3
#define TEXOBJ_WRAP_S      4
#define TEXOBJ_WRAP_T      5
#define TEXOBJ_MIPMAP      6
#define TEXOBJ_MIN_FILTER  7
#define TEXOBJ_MAG_FILTER  8
#define TEXOBJ_CI_FORMAT   16
#define TEXOBJ_TLUT_NAME   17
#define TEXOBJ_SIZE        22
#define TLUTOBJ_DATA       0
#define TLUTOBJ_FORMAT     1
#define TLUTOBJ_N_ENTRIES  2

typedef struct Acgc3dsTexBinding {
    C3D_Tex* tex;
    u32 format;
    u16 width;
    u16 height;
    u16 upload_width;
    u16 upload_height;
    u32 object_words[TEXOBJ_SIZE];
    u32 refreshed_frame;
    int valid;
} Acgc3dsTexBinding;
static Acgc3dsTexBinding s_tex_bindings[8];

typedef struct Acgc3dsTexCacheEntry {
    u32 data_ptr;
    u32 tlut_ptr;
    u32 tlut_name;
    u32 data_hash;
    u32 tlut_hash;
    u16 width;
    u16 height;
    u16 upload_width;
    u16 upload_height;
    u16 texture_width;
    u16 texture_height;
    u32 format;
    u32 tlut_format;
    u16 tlut_entries;
    u8 tlut_is_be;
    u8 wrap_s;
    u8 wrap_t;
    u8 min_filter;
    u8 mag_filter;
    u32 last_used_frame;
    C3D_Tex tex;
    int ready;
} Acgc3dsTexCacheEntry;

#define ACGC_3DS_TEX_CACHE_SIZE 512
static Acgc3dsTexCacheEntry s_tex_cache[ACGC_3DS_TEX_CACHE_SIZE];
static int s_tex_cache_next;
static u32 s_tex_cache_frame = 1;
typedef struct Acgc3dsTlut {
    const void* data;
    u32 format;
    u16 n_entries;
    int is_be;
} Acgc3dsTlut;
static Acgc3dsTlut s_tluts[16];

typedef struct Acgc3dsLightObjInternal {
    u32 padding[3];
    u32 color;
    f32 a0, a1, a2;
    f32 k0, k1, k2;
    f32 px, py, pz;
    f32 nx, ny, nz;
} Acgc3dsLightObjInternal;

typedef struct Acgc3dsLightState {
    f32 px, py, pz;
    f32 r, g, b, a;
} Acgc3dsLightState;

typedef struct Acgc3dsChannelState {
    GXBool enable;
    GXColorSrc amb_src;
    GXColorSrc mat_src;
    u32 light_mask;
    GXDiffuseFn diff_fn;
    GXAttnFn attn_fn;
} Acgc3dsChannelState;

static u8 s_num_chans;
static Acgc3dsChannelState s_color0_channel;
static Acgc3dsChannelState s_alpha0_channel;
static GXColor s_channel_ambient;
static GXColor s_channel_material;
static Acgc3dsLightState s_lights[8];
static GXBool s_depth_test;
static GXCompare s_depth_func;
static GXBool s_depth_write;
static GXBlendMode s_blend_mode;
static GXBlendFactor s_blend_src;
static GXBlendFactor s_blend_dst;
static GXBool s_color_update;
static GXBool s_alpha_update;
static GXCompare s_alpha_comp0;
static u8 s_alpha_ref0;
static GXAlphaOp s_alpha_op;
static GXCompare s_alpha_comp1;
static u8 s_alpha_ref1;

static void gx_refresh_binding(GXTexMapID map);

static u32 gx_hash_bytes(const void* data, size_t size) {
    const u8* p = (const u8*)data;
    u32 h = 0x811c9dc5u;
    if (p == NULL) return 0;
    for (size_t i = 0; i < size; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

static u32 gx_hash_texture_bytes(const void* data, size_t size) {
    const u8* p = (const u8*)data;
    u32 h;
    if (p == NULL || size == 0) return 0;
    if (size <= 16384u) return gx_hash_bytes(data, size);

    /* Large static textures are common enough that hashing every byte on
     * every GXLoadTexObj is expensive on the 3DS. Hash both ends and regular
     * samples through the body; all dynamic actor textures take the full-hash
     * path above. */
    h = gx_hash_bytes(p, 4096u);
    for (size_t offset = 4096u; offset + 4096u < size; offset += 4096u) {
        h ^= p[offset];
        h *= 0x01000193u;
        h ^= p[offset + 2048u];
        h *= 0x01000193u;
    }
    for (size_t i = size - 4096u; i < size; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

static u32 gx_hash_mix_u32(u32 hash, u32 value) {
    for (int i = 0; i < 4; i++) {
        hash ^= (u8)(value >> (i * 8));
        hash *= 0x01000193u;
    }
    return hash;
}

static size_t gx_texture_data_size(u16 width, u16 height, u32 format) {
    size_t blocks_x;
    size_t blocks_y;
    switch (format) {
        case GX_TF_I4:
        case GX_TF_C4:
        case GX_TF_CMPR:
            blocks_x = ((size_t)width + 7u) / 8u;
            blocks_y = ((size_t)height + 7u) / 8u;
            return blocks_x * blocks_y * 32u;
        case GX_TF_I8:
        case GX_TF_IA4:
        case GX_TF_C8:
            blocks_x = ((size_t)width + 7u) / 8u;
            blocks_y = ((size_t)height + 3u) / 4u;
            return blocks_x * blocks_y * 32u;
        case GX_TF_IA8:
        case GX_TF_RGB565:
        case GX_TF_RGB5A3:
            blocks_x = ((size_t)width + 3u) / 4u;
            blocks_y = ((size_t)height + 3u) / 4u;
            return blocks_x * blocks_y * 32u;
        case GX_TF_RGBA8:
            blocks_x = ((size_t)width + 3u) / 4u;
            blocks_y = ((size_t)height + 3u) / 4u;
            return blocks_x * blocks_y * 64u;
        default:
            return 0;
    }
}

static GPU_TEXTURE_WRAP_PARAM gx_wrap_mode(GXTexWrapMode wrap) {
    switch (wrap) {
        case GX_REPEAT:
            return GPU_REPEAT;
        case GX_MIRROR:
            return GPU_MIRRORED_REPEAT;
        case GX_CLAMP:
        default:
            return GPU_CLAMP_TO_EDGE;
    }
}

static GPU_TEXTURE_FILTER_PARAM gx_filter_mode(GXTexFilter filter) {
    switch (filter) {
        case GX_LINEAR:
        case GX_LIN_MIP_NEAR:
        case GX_LIN_MIP_LIN:
            return GPU_LINEAR;
        case GX_NEAR:
        case GX_NEAR_MIP_NEAR:
        case GX_NEAR_MIP_LIN:
        default:
            return GPU_NEAREST;
    }
}

static size_t gx_upload_size(size_t v) {
    size_t out = 8;
    if (v == 0 || v > 1024) return 0;
    while (out < v) out <<= 1;
    return out;
}

static void gx_swizzle_rgba8_3ds(const u8* src, u8* dst, int w, int h, int tex_w, int tex_h) {
    static const u8 morton[64] = {
        0, 1, 4, 5, 16, 17, 20, 21,
        2, 3, 6, 7, 18, 19, 22, 23,
        8, 9, 12, 13, 24, 25, 28, 29,
        10, 11, 14, 15, 26, 27, 30, 31,
        32, 33, 36, 37, 48, 49, 52, 53,
        34, 35, 38, 39, 50, 51, 54, 55,
        40, 41, 44, 45, 56, 57, 60, 61,
        42, 43, 46, 47, 58, 59, 62, 63
    };
    memset(dst, 0, (size_t)tex_w * (size_t)tex_h * 4);
    for (int y = 0; y < tex_h; y++) {
        for (int x = 0; x < tex_w; x++) {
            int tile_x = x & ~7;
            int tile_y = y & ~7;
            int in_tile = morton[(y & 7) * 8 + (x & 7)];
            /* tile_x is measured in pixels, while a horizontal 8x8 tile
             * occupies 64 texels in PICA memory. */
            size_t dst_idx = (((size_t)tile_y * (size_t)tex_w +
                               (size_t)tile_x * 8u) + (size_t)in_tile) * 4;
            int sx = x < w ? x : w - 1;
            int sy = y < h ? y : h - 1;
            const u8* s = &src[((size_t)sy * (size_t)w + (size_t)sx) * 4];
            /*
             * PICA200 RGBA8 texture memory is consumed as ABGR bytes.  The
             * GameCube decoders above produce ordinary RGBA, so swap here
             * while we tile.  Leaving this as RGBA produces the rainbow-tint
             * corruption seen on most textured assets.
             */
            dst[dst_idx + 0] = s[3];
            dst[dst_idx + 1] = s[2];
            dst[dst_idx + 2] = s[1];
            dst[dst_idx + 3] = s[0];
        }
    }
}

static void gx_identity(void) {
    memset(s_projection, 0, sizeof(s_projection));
    for (int i = 0; i < 4; ++i) s_projection[i][i] = 1.0f;
    memset(s_position, 0, sizeof(s_position));
    memset(s_normal, 0, sizeof(s_normal));
    for (int i = 0; i < 10; ++i) {
        s_position[i][0][0] = 1.0f;
        s_position[i][1][1] = 1.0f;
        s_position[i][2][2] = 1.0f;
        s_normal[i][0][0] = 1.0f;
        s_normal[i][1][1] = 1.0f;
        s_normal[i][2][2] = 1.0f;
    }
    memset(s_texture_mtx, 0, sizeof(s_texture_mtx));
    for (int i = 0; i < 10; ++i) {
        s_texture_mtx[i][0][0] = 1.0f;
        s_texture_mtx[i][1][1] = 1.0f;
        s_texture_mtx[i][2][2] = 1.0f;
    }
    memset(&s_current, 0, sizeof(s_current));
    s_current.r = s_current.g = s_current.b = s_current.a = 255;
}

static void gx_tev_stage_set_mode(Acgc3dsTevStage* stage, GXTevMode mode) {
    switch (mode) {
        case GX_MODULATE:
            stage->color[0]=GX_CC_ZERO; stage->color[1]=GX_CC_TEXC;
            stage->color[2]=GX_CC_RASC; stage->color[3]=GX_CC_ZERO;
            stage->alpha[0]=GX_CA_ZERO; stage->alpha[1]=GX_CA_TEXA;
            stage->alpha[2]=GX_CA_RASA; stage->alpha[3]=GX_CA_ZERO;
            break;
        case GX_DECAL:
            stage->color[0]=GX_CC_RASC; stage->color[1]=GX_CC_TEXC;
            stage->color[2]=GX_CC_TEXA; stage->color[3]=GX_CC_ZERO;
            stage->alpha[0]=GX_CA_ZERO; stage->alpha[1]=GX_CA_ZERO;
            stage->alpha[2]=GX_CA_ZERO; stage->alpha[3]=GX_CA_RASA;
            break;
        case GX_BLEND:
            stage->color[0]=GX_CC_ONE; stage->color[1]=GX_CC_RASC;
            stage->color[2]=GX_CC_TEXC; stage->color[3]=GX_CC_ZERO;
            stage->alpha[0]=GX_CA_ZERO; stage->alpha[1]=GX_CA_TEXA;
            stage->alpha[2]=GX_CA_RASA; stage->alpha[3]=GX_CA_ZERO;
            break;
        case GX_PASSCLR:
            stage->color[0]=GX_CC_ZERO; stage->color[1]=GX_CC_ZERO;
            stage->color[2]=GX_CC_ZERO; stage->color[3]=GX_CC_RASC;
            stage->alpha[0]=GX_CA_ZERO; stage->alpha[1]=GX_CA_ZERO;
            stage->alpha[2]=GX_CA_ZERO; stage->alpha[3]=GX_CA_RASA;
            break;
        case GX_REPLACE:
        default:
            stage->color[0]=GX_CC_ZERO; stage->color[1]=GX_CC_ZERO;
            stage->color[2]=GX_CC_ZERO; stage->color[3]=GX_CC_TEXC;
            stage->alpha[0]=GX_CA_ZERO; stage->alpha[1]=GX_CA_ZERO;
            stage->alpha[2]=GX_CA_ZERO; stage->alpha[3]=GX_CA_TEXA;
            break;
    }
    stage->color_op = stage->alpha_op = GX_TEV_ADD;
    stage->color_bias = stage->alpha_bias = GX_TB_ZERO;
    stage->color_scale = stage->alpha_scale = GX_CS_SCALE_1;
    stage->color_clamp = stage->alpha_clamp = GX_TRUE;
    stage->color_out = stage->alpha_out = GX_TEVPREV;
}

static void gx_state_defaults(void) {
    memset(s_vtx_desc, 0, sizeof(s_vtx_desc));
    memset(s_vtx_cnt, 0, sizeof(s_vtx_cnt));
    memset(s_vtx_type, 0, sizeof(s_vtx_type));
    memset(s_vtx_frac, 0, sizeof(s_vtx_frac));
    s_vtx_desc[GX_VA_POS] = GX_DIRECT;
    s_vtx_desc[GX_VA_CLR0] = GX_DIRECT;
    s_vtx_cnt[GX_VTXFMT0][GX_VA_POS] = GX_POS_XYZ;
    s_vtx_type[GX_VTXFMT0][GX_VA_POS] = GX_F32;
    s_vtx_cnt[GX_VTXFMT0][GX_VA_CLR0] = GX_CLR_RGBA;
    s_vtx_type[GX_VTXFMT0][GX_VA_CLR0] = GX_RGBA8;
    memset(s_tex_bindings, 0, sizeof(s_tex_bindings));
    s_bound_texture = NULL;
    s_bound_texture_format = 0;
    s_bound_texture_width = 0;
    s_bound_texture_height = 0;
    s_bound_texture_upload_width = 0;
    s_bound_texture_upload_height = 0;
    s_active_texmap = GX_TEXMAP_NULL;
    s_active_texcoord = GX_TEXCOORD0;
    s_active_tev_stage = 0;
    s_num_tev_stages = 1;
    for (int i = 0; i < GX_MAX_TEVSTAGE; i++) {
        memset(&s_tev_stages[i], 0, sizeof(s_tev_stages[i]));
        gx_tev_stage_set_mode(&s_tev_stages[i], i == 0 ? GX_REPLACE : GX_PASSCLR);
        if (i != 0) {
            s_tev_stages[i].color[3] = GX_CC_CPREV;
            s_tev_stages[i].alpha[3] = GX_CA_APREV;
        }
        s_tev_stages[i].texcoord = GX_TEXCOORD_NULL;
        s_tev_stages[i].texmap = GX_TEXMAP_NULL;
        s_tev_stages[i].channel = GX_COLOR_NULL;
    }
    for (int i = 0; i < 8; ++i) s_texgen_mtx[i] = GX_IDENTITY;
    s_num_chans = 1;
    s_color0_channel.enable = GX_FALSE;
    s_color0_channel.amb_src = GX_SRC_REG;
    s_color0_channel.mat_src = GX_SRC_VTX;
    s_color0_channel.light_mask = GX_LIGHT_NULL;
    s_color0_channel.diff_fn = GX_DF_NONE;
    s_color0_channel.attn_fn = GX_AF_NONE;
    s_alpha0_channel = s_color0_channel;
    s_channel_ambient = (GXColor){ 0, 0, 0, 0 };
    s_channel_material = (GXColor){ 255, 255, 255, 255 };
    for (int i = 0; i < 4; i++) s_tev_colors[i] = (GXColor){ 255, 255, 255, 255 };
    memset(s_lights, 0, sizeof(s_lights));
    s_depth_test = GX_TRUE;
    s_depth_func = GX_LEQUAL;
    s_depth_write = GX_TRUE;
    s_blend_mode = GX_BM_NONE;
    s_blend_src = GX_BL_ONE;
    s_blend_dst = GX_BL_ZERO;
    s_color_update = GX_TRUE;
    s_alpha_update = GX_TRUE;
    s_alpha_comp0 = GX_ALWAYS;
    s_alpha_ref0 = 0;
    s_alpha_op = GX_AOP_AND;
    s_alpha_comp1 = GX_ALWAYS;
    s_alpha_ref1 = 0;
}

/* PICA200 exposes one alpha comparison while GX combines two. Collapse the
 * combinations used by the game (ALWAYS and paired threshold comparisons)
 * to an equivalent single test. Unknown combinations conservatively leave
 * alpha testing disabled instead of rejecting visible pixels. */
static void gx_resolve_alpha_test(int* enabled, int* func, int* ref) {
    GXCompare comp0 = s_alpha_comp0;
    GXCompare comp1 = s_alpha_comp1;
    u8 ref0 = s_alpha_ref0;
    u8 ref1 = s_alpha_ref1;

    *enabled = 1;
    *func = GX_ALWAYS;
    *ref = 0;

    if (s_alpha_op == GX_AOP_AND) {
        if (comp0 == GX_NEVER || comp1 == GX_NEVER) {
            *func = GX_NEVER;
            return;
        }
        if (comp0 == GX_ALWAYS) {
            comp0 = comp1;
            ref0 = ref1;
        } else if (comp1 != GX_ALWAYS) {
            if (comp0 != comp1) {
                *enabled = 0;
                return;
            }
            if (comp0 == GX_GREATER || comp0 == GX_GEQUAL) {
                if (ref1 > ref0) ref0 = ref1;
            } else if (comp0 == GX_LESS || comp0 == GX_LEQUAL) {
                if (ref1 < ref0) ref0 = ref1;
            } else if (comp0 == GX_EQUAL && ref0 != ref1) {
                *func = GX_NEVER;
                return;
            } else if (comp0 == GX_NEQUAL && ref0 != ref1) {
                *enabled = 0;
                return;
            }
        }
    } else if (s_alpha_op == GX_AOP_OR) {
        if (comp0 == GX_ALWAYS || comp1 == GX_ALWAYS) {
            *enabled = 0;
            return;
        }
        if (comp0 == GX_NEVER) {
            comp0 = comp1;
            ref0 = ref1;
        } else if (comp1 != GX_NEVER) {
            if (comp0 != comp1) {
                *enabled = 0;
                return;
            }
            if (comp0 == GX_GREATER || comp0 == GX_GEQUAL) {
                if (ref1 < ref0) ref0 = ref1;
            } else if (comp0 == GX_LESS || comp0 == GX_LEQUAL) {
                if (ref1 > ref0) ref0 = ref1;
            } else if (comp0 == GX_EQUAL && ref0 != ref1) {
                *enabled = 0;
                return;
            }
        }
    } else {
        *enabled = 0;
        return;
    }

    if (comp0 == GX_ALWAYS) {
        *enabled = 0;
        return;
    }
    *func = comp0;
    *ref = ref0;
}

static void gx_select_texmap(GXTexMapID map) {
    s_active_texmap = map;
    if (map >= GX_TEXMAP0 && map <= GX_TEXMAP7) {
        Acgc3dsTexBinding* binding = &s_tex_bindings[map];
        s_bound_texture = binding->tex;
        s_bound_texture_format = binding->format;
        s_bound_texture_width = binding->width;
        s_bound_texture_height = binding->height;
        s_bound_texture_upload_width = binding->upload_width;
        s_bound_texture_upload_height = binding->upload_height;
    } else {
        s_bound_texture = NULL;
        s_bound_texture_format = 0;
        s_bound_texture_width = 0;
        s_bound_texture_height = 0;
        s_bound_texture_upload_width = 0;
        s_bound_texture_upload_height = 0;
    }
}

static void gx_resolve_tev_texture(void) {
    int count = s_num_tev_stages;
    if (count > GX_MAX_TEVSTAGE) count = GX_MAX_TEVSTAGE;
    s_active_tev_stage = 0;
    for (int i = 0; i < count; i++) {
        Acgc3dsTevStage* stage = &s_tev_stages[i];
        if (stage->texmap >= GX_TEXMAP0 && stage->texmap <= GX_TEXMAP7) {
            s_active_tev_stage = i;
            s_active_texcoord = stage->texcoord < GX_MAX_TEXCOORD ? stage->texcoord : GX_TEXCOORD0;
            gx_select_texmap(stage->texmap);
            return;
        }
    }
    if (count > 0 && s_tev_stages[0].texcoord < GX_MAX_TEXCOORD) {
        s_active_texcoord = s_tev_stages[0].texcoord;
    }
    gx_select_texmap(GX_TEXMAP_NULL);
}

/* Return the texture map that supplies an I4 mask to the active TEV program.
 * Animal Crossing uses this path for both the main character atlas and the
 * small cursor/arrow glyphs.  Do not rely on s_active_texmap here: emu64 can
 * describe the original two-cycle N64 combiner with more than one GX stage,
 * and later state changes may leave a different map selected even though the
 * font coverage still comes from an earlier stage. */
static GXTexMapID gx_find_tev_i4_texmap(void) {
    int count = s_num_tev_stages;
    if (count > GX_MAX_TEVSTAGE) count = GX_MAX_TEVSTAGE;

    for (int i = 0; i < count; i++) {
        GXTexMapID map = s_tev_stages[i].texmap;
        if (map >= GX_TEXMAP0 && map <= GX_TEXMAP7 &&
            s_tex_bindings[map].valid &&
            s_tex_bindings[map].format == GX_TF_I4) {
            return map;
        }
    }
    return GX_TEXMAP_NULL;
}

static void gx_refresh_tev_bindings(void) {
    int count = s_num_tev_stages;
    if (count > GX_MAX_TEVSTAGE) count = GX_MAX_TEVSTAGE;

    for (int i = 0; i < count; i++) {
        GXTexMapID map = s_tev_stages[i].texmap;
        if (map >= GX_TEXMAP0 && map <= GX_TEXMAP7) {
            gx_refresh_binding(map);
        }
    }
}

static int gx_tev_color_is(const Acgc3dsTevStage* stage,
                           GXTevColorArg a, GXTevColorArg b,
                           GXTevColorArg c, GXTevColorArg d) {
    return stage->color[0] == a && stage->color[1] == b &&
           stage->color[2] == c && stage->color[3] == d;
}

static int gx_tev_alpha_is(const Acgc3dsTevStage* stage,
                           GXTevAlphaArg a, GXTevAlphaArg b,
                           GXTevAlphaArg c, GXTevAlphaArg d) {
    return stage->alpha[0] == a && stage->alpha[1] == b &&
           stage->alpha[2] == c && stage->alpha[3] == d;
}

static int gx_tev_stage_is_passthrough(const Acgc3dsTevStage* stage) {
    return stage->color_op == GX_TEV_ADD && stage->alpha_op == GX_TEV_ADD &&
           stage->color_bias == GX_TB_ZERO && stage->alpha_bias == GX_TB_ZERO &&
           stage->color_scale == GX_CS_SCALE_1 && stage->alpha_scale == GX_CS_SCALE_1 &&
           stage->color_out == GX_TEVPREV && stage->alpha_out == GX_TEVPREV &&
           gx_tev_color_is(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV) &&
           gx_tev_alpha_is(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
}

static int gx_tev_stage_uses_color(const Acgc3dsTevStage* stage,
                                   GXTevColorArg arg) {
    return stage->color[0] == arg || stage->color[1] == arg ||
           stage->color[2] == arg || stage->color[3] == arg;
}

static int gx_tev_stage_uses_alpha(const Acgc3dsTevStage* stage,
                                   GXTevAlphaArg arg) {
    return stage->alpha[0] == arg || stage->alpha[1] == arg ||
           stage->alpha[2] == arg || stage->alpha[3] == arg;
}

static int gx_tev_alpha_is_texture_mask(const Acgc3dsTevStage* stage) {
    return gx_tev_alpha_is(stage, GX_CA_ZERO, GX_CA_ZERO,
                           GX_CA_ZERO, GX_CA_TEXA) ||
           gx_tev_alpha_is(stage, GX_CA_ZERO, GX_CA_TEXA,
                           GX_CA_A1, GX_CA_ZERO);
}

static int gx_tev_remaining_stages_passthrough(int active_stage) {
    for (int i = 0; i < s_num_tev_stages && i < GX_MAX_TEVSTAGE; i++) {
        if (i != active_stage && !gx_tev_stage_is_passthrough(&s_tev_stages[i])) {
            return 0;
        }
    }
    return 1;
}

static Acgc3dsTevPreset gx_tev_preset(void) {
    const Acgc3dsTevStage* stage = &s_tev_stages[s_active_tev_stage];
    if (s_num_tev_stages == 0) return ACGC_3DS_TEV_PASSCLR;
    if (gx_find_tev_i4_texmap() != GX_TEXMAP_NULL) {
        int uses_primitive_color = 0;
        int uses_primitive_alpha = 0;
        int uses_texture_alpha = 0;
        for (int i = 0; i < s_num_tev_stages && i < GX_MAX_TEVSTAGE; i++) {
            uses_primitive_color |= gx_tev_stage_uses_color(&s_tev_stages[i], GX_CC_C1);
            uses_primitive_alpha |= gx_tev_stage_uses_alpha(&s_tev_stages[i], GX_CA_A1);
            uses_texture_alpha |= gx_tev_stage_uses_alpha(&s_tev_stages[i], GX_CA_TEXA);
        }
        /* Animal Crossing's font atlas is I4. Font vertices deliberately have
         * no vertex color; the N64 combiner supplies RGB from the primitive
         * color and uses texture intensity as an alpha mask. The generic 3DS
         * fallback modulates by vertex color, making every glyph transparent. */
        if (uses_primitive_color && uses_primitive_alpha && uses_texture_alpha) {
            return ACGC_3DS_TEV_FONT_MASK;
        }
    }
    if (s_num_tev_stages == 2 &&
        s_tev_stages[0].texmap == GX_TEXMAP0 &&
        s_tev_stages[1].texmap == GX_TEXMAP1 &&
        gx_tev_color_is(&s_tev_stages[0], GX_CC_C2, GX_CC_C1, GX_CC_TEXC, GX_CC_ZERO) &&
        gx_tev_alpha_is(&s_tev_stages[0], GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA) &&
        gx_tev_color_is(&s_tev_stages[1], GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV) &&
        gx_tev_alpha_is(&s_tev_stages[1], GX_CA_ZERO, GX_CA_APREV, GX_CA_TEXA, GX_CA_TEXA)) {
        return ACGC_3DS_TEV_SPOTLIGHT;
    }
    if (s_num_tev_stages == 2 &&
        s_tev_stages[0].texmap == GX_TEXMAP0 &&
        s_tev_stages[1].texmap == GX_TEXMAP1 &&
        gx_tev_color_is(&s_tev_stages[0], GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC) &&
        gx_tev_alpha_is(&s_tev_stages[0], GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO) &&
        gx_tev_color_is(&s_tev_stages[1], GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV) &&
        gx_tev_alpha_is(&s_tev_stages[1], GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA)) {
        return ACGC_3DS_TEV_MASKED;
    }

    /* Most HUD art is intensity/alpha artwork colored by the N64 primitive
     * and environment registers. Treating these combiners as the generic
     * texture * vertex-color fallback turns them pale and partially
     * transparent because HUD vertices normally carry white raster color. */
    if (gx_tev_remaining_stages_passthrough(s_active_tev_stage) &&
        gx_tev_alpha_is_texture_mask(stage)) {
        if (gx_tev_color_is(stage, GX_CC_ZERO, GX_CC_ZERO,
                            GX_CC_ZERO, GX_CC_C1)) {
            return ACGC_3DS_TEV_HUD_TINT;
        }
        if (gx_tev_color_is(stage, GX_CC_C2, GX_CC_C1,
                            GX_CC_TEXC, GX_CC_ZERO)) {
            return ACGC_3DS_TEV_HUD_GRADIENT;
        }
    }

    if (!gx_tev_remaining_stages_passthrough(s_active_tev_stage)) {
        return ACGC_3DS_TEV_FALLBACK;
    }
    if (stage->color_op != GX_TEV_ADD || stage->alpha_op != GX_TEV_ADD ||
        stage->color_bias != GX_TB_ZERO || stage->alpha_bias != GX_TB_ZERO ||
        stage->color_scale != GX_CS_SCALE_1 || stage->alpha_scale != GX_CS_SCALE_1 ||
        stage->color_out != GX_TEVPREV || stage->alpha_out != GX_TEVPREV) {
        return ACGC_3DS_TEV_FALLBACK;
    }
    if (gx_tev_color_is(stage, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO) &&
        gx_tev_alpha_is(stage, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO)) {
        return ACGC_3DS_TEV_MODULATE;
    }
    if (gx_tev_color_is(stage, GX_CC_RASC, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO) &&
        gx_tev_alpha_is(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA)) {
        return ACGC_3DS_TEV_DECAL;
    }
    if (gx_tev_color_is(stage, GX_CC_ONE, GX_CC_RASC, GX_CC_TEXC, GX_CC_ZERO) &&
        gx_tev_alpha_is(stage, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO)) {
        return ACGC_3DS_TEV_BLEND;
    }
    if (gx_tev_color_is(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC) &&
        gx_tev_alpha_is(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA)) {
        return ACGC_3DS_TEV_REPLACE;
    }
    if (gx_tev_color_is(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC) &&
        gx_tev_alpha_is(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA)) {
        return ACGC_3DS_TEV_PASSCLR;
    }
    return ACGC_3DS_TEV_FALLBACK;
}

static u16 gx_read_be16(const u8* p) {
    return (u16)(((u16)p[0] << 8) | p[1]);
}

static u32 gx_read_be32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static f32 gx_read_be_f32(const u8* p) {
    union {
        u32 u;
        f32 f;
    } v;
    v.u = gx_read_be32(p);
    return v.f;
}

static void gx_decode_rgb5a3(u16 val, u8* r, u8* g, u8* b, u8* a) {
    if (val & 0x8000) {
        *r = (u8)(((val >> 10) & 0x1f) * 255 / 31);
        *g = (u8)(((val >> 5) & 0x1f) * 255 / 31);
        *b = (u8)((val & 0x1f) * 255 / 31);
        *a = 255;
    } else {
        *a = (u8)(((val >> 12) & 0x07) * 255 / 7);
        *r = (u8)(((val >> 8) & 0x0f) * 255 / 15);
        *g = (u8)(((val >> 4) & 0x0f) * 255 / 15);
        *b = (u8)((val & 0x0f) * 255 / 15);
    }
}

static void gx_build_palette(const Acgc3dsTlut* tlut, u8 palette[256][4]) {
    for (int i = 0; i < 256; i++) {
        palette[i][0] = palette[i][1] = palette[i][2] = (u8)i;
        palette[i][3] = 255;
    }
    if (tlut == NULL || tlut->data == NULL || tlut->n_entries == 0) return;
    for (int i = 0; i < tlut->n_entries && i < 256; i++) {
        const u8* p = (const u8*)tlut->data + i * 2;
        u16 val = tlut->is_be ? gx_read_be16(p) : ((const u16*)tlut->data)[i];
        if (tlut->format == GX_TL_RGB5A3) {
            gx_decode_rgb5a3(val, &palette[i][0], &palette[i][1], &palette[i][2], &palette[i][3]);
        } else if (tlut->format == GX_TL_RGB565) {
            palette[i][0] = (u8)(((val >> 11) & 0x1f) * 255 / 31);
            palette[i][1] = (u8)(((val >> 5) & 0x3f) * 255 / 63);
            palette[i][2] = (u8)((val & 0x1f) * 255 / 31);
            palette[i][3] = 255;
        } else {
            palette[i][0] = palette[i][1] = palette[i][2] = tlut->is_be ? (u8)(val >> 8) : (u8)(val & 0xff);
            palette[i][3] = tlut->is_be ? (u8)(val & 0xff) : (u8)(val >> 8);
        }
    }
}

static void gx_decode_texture(const u8* src, u8* dst, int w, int h, u32 fmt, const Acgc3dsTlut* tlut) {
    u8 palette[256][4];
    memset(dst, 0xff, (size_t)w * (size_t)h * 4);
    if (src == NULL || w <= 0 || h <= 0) return;
    if (fmt == GX_TF_C4 || fmt == GX_TF_C8) gx_build_palette(tlut, palette);

    if (fmt == GX_TF_I4) {
        int bw = (w + 7) / 8, bh = (h + 7) / 8;
        for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++)
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x += 2) {
            u8 val = *src++;
            int px0 = bx * 8 + x, px1 = px0 + 1, py = by * 8 + y;
            u8 i0 = (val >> 4) | (val & 0xf0), i1 = (val & 0x0f) | ((val & 0x0f) << 4);
            if (px0 < w && py < h) { u8* d = &dst[(py * w + px0) * 4]; d[0] = d[1] = d[2] = d[3] = i0; }
            if (px1 < w && py < h) { u8* d = &dst[(py * w + px1) * 4]; d[0] = d[1] = d[2] = d[3] = i1; }
        }
    } else if (fmt == GX_TF_I8) {
        int bw = (w + 7) / 8, bh = (h + 3) / 4;
        for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++)
        for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++) {
            u8 val = *src++;
            int px = bx * 8 + x, py = by * 4 + y;
            if (px < w && py < h) { u8* d = &dst[(py * w + px) * 4]; d[0] = d[1] = d[2] = d[3] = val; }
        }
    } else if (fmt == GX_TF_IA4) {
        int bw = (w + 7) / 8, bh = (h + 3) / 4;
        for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++)
        for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++) {
            u8 val = *src++;
            int px = bx * 8 + x, py = by * 4 + y;
            if (px < w && py < h) {
                u8 a = (val >> 4) | (val & 0xf0), i = (val & 0x0f) | ((val & 0x0f) << 4);
                u8* d = &dst[(py * w + px) * 4]; d[0] = d[1] = d[2] = i; d[3] = a;
            }
        }
    } else if (fmt == GX_TF_IA8) {
        int bw = (w + 3) / 4, bh = (h + 3) / 4;
        for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++)
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            u8 a = *src++, i = *src++;
            int px = bx * 4 + x, py = by * 4 + y;
            if (px < w && py < h) { u8* d = &dst[(py * w + px) * 4]; d[0] = d[1] = d[2] = i; d[3] = a; }
        }
    } else if (fmt == GX_TF_RGB565 || fmt == GX_TF_RGB5A3) {
        int bw = (w + 3) / 4, bh = (h + 3) / 4;
        for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++)
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            u16 val = gx_read_be16(src); src += 2;
            int px = bx * 4 + x, py = by * 4 + y;
            if (px < w && py < h) {
                u8* d = &dst[(py * w + px) * 4];
                if (fmt == GX_TF_RGB565) {
                    d[0] = (u8)(((val >> 11) & 0x1f) * 255 / 31);
                    d[1] = (u8)(((val >> 5) & 0x3f) * 255 / 63);
                    d[2] = (u8)((val & 0x1f) * 255 / 31);
                    d[3] = 255;
                } else {
                    gx_decode_rgb5a3(val, &d[0], &d[1], &d[2], &d[3]);
                }
            }
        }
    } else if (fmt == GX_TF_RGBA8) {
        int bw = (w + 3) / 4, bh = (h + 3) / 4;
        for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
            u8 ar[16][2];
            for (int i = 0; i < 16; i++) { ar[i][0] = *src++; ar[i][1] = *src++; }
            for (int i = 0; i < 16; i++) {
                int x = i % 4, y = i / 4, px = bx * 4 + x, py = by * 4 + y;
                u8 g = *src++, b = *src++;
                if (px < w && py < h) {
                    u8* d = &dst[(py * w + px) * 4];
                    d[0] = ar[i][1]; d[1] = g; d[2] = b; d[3] = ar[i][0];
                }
            }
        }
    } else if (fmt == GX_TF_C4) {
        int bw = (w + 7) / 8, bh = (h + 7) / 8;
        for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++)
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x += 2) {
            u8 val = *src++;
            int px0 = bx * 8 + x, px1 = px0 + 1, py = by * 8 + y;
            u8 ci0 = (val >> 4) & 0x0f, ci1 = val & 0x0f;
            if (px0 < w && py < h) memcpy(&dst[(py * w + px0) * 4], palette[ci0], 4);
            if (px1 < w && py < h) memcpy(&dst[(py * w + px1) * 4], palette[ci1], 4);
        }
    } else if (fmt == GX_TF_C8) {
        int bw = (w + 7) / 8, bh = (h + 3) / 4;
        for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++)
        for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++) {
            u8 ci = *src++;
            int px = bx * 8 + x, py = by * 4 + y;
            if (px < w && py < h) memcpy(&dst[(py * w + px) * 4], palette[ci], 4);
        }
    } else if (fmt == GX_TF_CMPR) {
        int bw = (w + 7) / 8, bh = (h + 7) / 8;
        for (int by = 0; by < bh; by++) {
            for (int bx = 0; bx < bw; bx++) {
                for (int sub = 0; sub < 4; sub++) {
                    int sx = (sub & 1) * 4, sy = (sub >> 1) * 4;
                    u16 c0 = gx_read_be16(src);
                    u16 c1 = gx_read_be16(src + 2);
                    u8 colors[4][4];
                    src += 4;

                    colors[0][0] = (u8)(((c0 >> 11) & 0x1f) * 255 / 31);
                    colors[0][1] = (u8)(((c0 >> 5) & 0x3f) * 255 / 63);
                    colors[0][2] = (u8)((c0 & 0x1f) * 255 / 31);
                    colors[0][3] = 255;
                    colors[1][0] = (u8)(((c1 >> 11) & 0x1f) * 255 / 31);
                    colors[1][1] = (u8)(((c1 >> 5) & 0x3f) * 255 / 63);
                    colors[1][2] = (u8)((c1 & 0x1f) * 255 / 31);
                    colors[1][3] = 255;
                    if (c0 > c1) {
                        for (int c = 0; c < 3; c++) {
                            colors[2][c] = (u8)((2 * colors[0][c] + colors[1][c]) / 3);
                            colors[3][c] = (u8)((colors[0][c] + 2 * colors[1][c]) / 3);
                        }
                        colors[2][3] = colors[3][3] = 255;
                    } else {
                        for (int c = 0; c < 3; c++) {
                            colors[2][c] = (u8)((colors[0][c] + colors[1][c]) / 2);
                        }
                        colors[2][3] = 255;
                        colors[3][0] = colors[3][1] = colors[3][2] = colors[3][3] = 0;
                    }

                    for (int y = 0; y < 4; y++) {
                        u8 row = *src++;
                        for (int x = 0; x < 4; x++) {
                            int ci = (row >> (6 - x * 2)) & 3;
                            int px = bx * 8 + sx + x;
                            int py = by * 8 + sy + y;
                            if (px < w && py < h) {
                                memcpy(&dst[((size_t)py * (size_t)w + (size_t)px) * 4], colors[ci], 4);
                            }
                        }
                    }
                }
            }
        }
    }
}

static void gx_evict_texture_entry(Acgc3dsTexCacheEntry* entry) {
    if (entry->ready) {
        for (int i = 0; i < 8; i++) {
            if (s_tex_bindings[i].tex == &entry->tex) s_tex_bindings[i].tex = NULL;
        }
        if (s_bound_texture == &entry->tex) s_bound_texture = NULL;
        C3D_TexDelete(&entry->tex);
    }
    memset(entry, 0, sizeof(*entry));
}

static C3D_Tex* gx_upload_texture(const GXTexObj* obj, u16* out_width,
                                  u16* out_height, u16* out_upload_width,
                                  u16* out_upload_height) {
    const u32* o = (const u32*)obj;
    u32 data_ptr = o[TEXOBJ_IMAGE_PTR];
    u16 width = (u16)o[TEXOBJ_WIDTH];
    u16 height = (u16)o[TEXOBJ_HEIGHT];
    u32 format = o[TEXOBJ_FORMAT];
    u32 tlut_name = (format == GX_TF_C4 || format == GX_TF_C8) ? o[TEXOBJ_TLUT_NAME] : 0xffffffffu;
    Acgc3dsTlut* tlut = tlut_name < 16 ? &s_tluts[tlut_name] : NULL;
    u32 tlut_ptr = tlut != NULL ? (u32)(uintptr_t)tlut->data : 0;
    size_t data_size = gx_texture_data_size(width, height, format);
    u32 data_hash;
    u32 tlut_hash = 0;
    size_t upload_width = gx_upload_size(width);
    size_t upload_height = gx_upload_size(height);
    u16 tex_width;
    u16 tex_height;
    u32 wrap_s = o[TEXOBJ_WRAP_S];
    u32 wrap_t = o[TEXOBJ_WRAP_T];
    u32 min_filter = o[TEXOBJ_MIN_FILTER];
    u32 mag_filter = o[TEXOBJ_MAG_FILTER];
    u8* rgba;
    int replacement_width = 0;
    int replacement_height = 0;
    Acgc3dsTexCacheEntry* e;
    int cache_index = -1;

    if (data_ptr == 0 || data_size == 0 || upload_width == 0 || upload_height == 0) return NULL;
    tex_width = (u16)upload_width;
    tex_height = (u16)upload_height;

    /* A texture can be selected many times in one display list. Once it has
     * been validated this frame, pointer and GX metadata are sufficient; game
     * texture producers finish writing before issuing their draw commands. */
    for (int i = 0; i < ACGC_3DS_TEX_CACHE_SIZE; i++) {
        e = &s_tex_cache[i];
        if (e->ready && e->last_used_frame == s_tex_cache_frame &&
            e->data_ptr == data_ptr && e->tlut_ptr == tlut_ptr &&
            e->tlut_name == tlut_name && e->width == width &&
            e->height == height && e->format == format &&
            e->tlut_format == (tlut != NULL ? tlut->format : 0) &&
            e->tlut_entries == (tlut != NULL ? tlut->n_entries : 0) &&
            e->tlut_is_be == (u8)(tlut != NULL ? tlut->is_be : 0) &&
            e->wrap_s == (u8)wrap_s && e->wrap_t == (u8)wrap_t &&
            e->min_filter == (u8)min_filter && e->mag_filter == (u8)mag_filter) {
            if (out_width != NULL) *out_width = e->texture_width;
            if (out_height != NULL) *out_height = e->texture_height;
            if (out_upload_width != NULL) *out_upload_width = e->upload_width;
            if (out_upload_height != NULL) *out_upload_height = e->upload_height;
            return &e->tex;
        }
    }
    data_hash = gx_hash_texture_bytes((const void*)(uintptr_t)data_ptr, data_size);
    if (tlut != NULL) {
        tlut_hash = gx_hash_bytes(tlut->data, (size_t)tlut->n_entries * 2u);
        tlut_hash = gx_hash_mix_u32(tlut_hash, tlut->format);
        tlut_hash = gx_hash_mix_u32(tlut_hash, tlut->n_entries);
        tlut_hash = gx_hash_mix_u32(tlut_hash, (u32)tlut->is_be);
    }
    for (int i = 0; i < ACGC_3DS_TEX_CACHE_SIZE; i++) {
        e = &s_tex_cache[i];
        if (e->ready && e->data_ptr == data_ptr && e->tlut_ptr == tlut_ptr &&
            e->data_hash == data_hash && e->tlut_hash == tlut_hash &&
            e->tlut_name == tlut_name && e->width == width &&
            e->height == height && e->format == format &&
            e->tlut_format == (tlut != NULL ? tlut->format : 0) &&
            e->tlut_entries == (tlut != NULL ? tlut->n_entries : 0) &&
            e->tlut_is_be == (u8)(tlut != NULL ? tlut->is_be : 0) &&
            e->wrap_s == (u8)wrap_s && e->wrap_t == (u8)wrap_t &&
            e->min_filter == (u8)min_filter && e->mag_filter == (u8)mag_filter) {
            e->last_used_frame = s_tex_cache_frame;
            if (out_width != NULL) *out_width = e->texture_width;
            if (out_height != NULL) *out_height = e->texture_height;
            if (out_upload_width != NULL) *out_upload_width = e->upload_width;
            if (out_upload_height != NULL) *out_upload_height = e->upload_height;
            return &e->tex;
        }
    }

    /* Entries referenced by commands already queued this frame cannot be
     * destroyed until the next synchronized C3D frame. Recycle a stale entry
     * before growing the cache so linear memory usage tracks the maximum
     * number of textures needed by one frame, rather than every texture ever
     * observed across frames. */
    for (int n = 0; n < ACGC_3DS_TEX_CACHE_SIZE; n++) {
        int i = (s_tex_cache_next + n) % ACGC_3DS_TEX_CACHE_SIZE;
        if (s_tex_cache[i].ready && s_tex_cache[i].last_used_frame != s_tex_cache_frame) {
            cache_index = i;
            break;
        }
    }
    if (cache_index < 0) {
        for (int n = 0; n < ACGC_3DS_TEX_CACHE_SIZE; n++) {
            int i = (s_tex_cache_next + n) % ACGC_3DS_TEX_CACHE_SIZE;
            if (!s_tex_cache[i].ready) {
                cache_index = i;
                break;
            }
        }
    }
    if (cache_index < 0) return NULL;
    s_tex_cache_next = (cache_index + 1) % ACGC_3DS_TEX_CACHE_SIZE;
    e = &s_tex_cache[cache_index];
    gx_evict_texture_entry(e);
    rgba = acgc_3ds_texture_pack_lookup((const void*)(uintptr_t)data_ptr, data_size,
                                        width, height, format,
                                        tlut != NULL ? tlut->data : NULL,
                                        tlut != NULL ? tlut->n_entries : 0,
                                        tlut != NULL ? tlut->is_be : 0,
                                        &replacement_width, &replacement_height);
    if (rgba != NULL) {
        upload_width = gx_upload_size((size_t)replacement_width);
        upload_height = gx_upload_size((size_t)replacement_height);
        if (upload_width == 0 || upload_height == 0) {
            free(rgba);
            rgba = NULL;
        } else {
            tex_width = (u16)upload_width;
            tex_height = (u16)upload_height;
        }
    }
    if (!C3D_TexInit(&e->tex, tex_width, tex_height, GPU_RGBA8)) {
        int initialized = 0;
        /* Fragmentation or accumulated stale textures can make an allocation
         * fail even though none of those old textures are in flight. Release
         * additional old-frame entries and retry the requested allocation. */
        for (int n = 0; n < ACGC_3DS_TEX_CACHE_SIZE; n++) {
            int i = (s_tex_cache_next + n) % ACGC_3DS_TEX_CACHE_SIZE;
            Acgc3dsTexCacheEntry* victim = &s_tex_cache[i];
            if (victim == e || !victim->ready ||
                victim->last_used_frame == s_tex_cache_frame) continue;
            gx_evict_texture_entry(victim);
            memset(&e->tex, 0, sizeof(e->tex));
            if (C3D_TexInit(&e->tex, tex_width, tex_height, GPU_RGBA8)) {
                initialized = 1;
                break;
            }
        }
        if (!initialized) {
            free(rgba);
            memset(e, 0, sizeof(*e));
            return NULL;
        }
    }
    if (rgba == NULL) {
        rgba = (u8*)malloc((size_t)width * (size_t)height * 4);
        replacement_width = width;
        replacement_height = height;
        if (rgba == NULL) {
            C3D_TexDelete(&e->tex);
            return NULL;
        }
        gx_decode_texture((const u8*)(uintptr_t)data_ptr, rgba, width, height, format, tlut);
    }
    /* C3D_TexInit uses CPU-visible linear memory. Swizzle directly into the
     * final texture to avoid a second large allocation and a full-size upload
     * copy for every cache miss. */
    gx_swizzle_rgba8_3ds(rgba, (u8*)e->tex.data,
                         replacement_width, replacement_height, tex_width, tex_height);
    C3D_TexFlush(&e->tex);
    C3D_TexSetFilter(&e->tex, gx_filter_mode((GXTexFilter)mag_filter),
                     gx_filter_mode((GXTexFilter)min_filter));
    C3D_TexSetWrap(&e->tex, gx_wrap_mode((GXTexWrapMode)o[TEXOBJ_WRAP_S]),
                   gx_wrap_mode((GXTexWrapMode)o[TEXOBJ_WRAP_T]));
    free(rgba);
    e->data_ptr = data_ptr;
    e->tlut_ptr = tlut_ptr;
    e->tlut_name = tlut_name;
    e->data_hash = data_hash;
    e->tlut_hash = tlut_hash;
    e->width = width;
    e->height = height;
    e->upload_width = tex_width;
    e->upload_height = tex_height;
    e->texture_width = (u16)replacement_width;
    e->texture_height = (u16)replacement_height;
    e->format = format;
    e->tlut_format = tlut != NULL ? tlut->format : 0;
    e->tlut_entries = tlut != NULL ? tlut->n_entries : 0;
    e->tlut_is_be = (u8)(tlut != NULL ? tlut->is_be : 0);
    e->wrap_s = (u8)wrap_s;
    e->wrap_t = (u8)wrap_t;
    e->min_filter = (u8)min_filter;
    e->mag_filter = (u8)mag_filter;
    e->last_used_frame = s_tex_cache_frame;
    e->ready = 1;
    if (out_width != NULL) *out_width = (u16)replacement_width;
    if (out_height != NULL) *out_height = (u16)replacement_height;
    if (out_upload_width != NULL) *out_upload_width = tex_width;
    if (out_upload_height != NULL) *out_upload_height = tex_height;
    return &e->tex;
}

static void gx_refresh_binding(GXTexMapID map) {
    Acgc3dsTexBinding* binding;
    const u32* o;
    size_t upload_width;
    size_t upload_height;
    if (map < GX_TEXMAP0 || map > GX_TEXMAP7) return;
    binding = &s_tex_bindings[map];
    if (!binding->valid) return;
    if (binding->refreshed_frame == s_tex_cache_frame) {
        if (s_active_texmap == map) gx_select_texmap(map);
        return;
    }
    o = binding->object_words;
    upload_width = gx_upload_size((u16)o[TEXOBJ_WIDTH]);
    upload_height = gx_upload_size((u16)o[TEXOBJ_HEIGHT]);
    binding->width = (u16)o[TEXOBJ_WIDTH];
    binding->height = (u16)o[TEXOBJ_HEIGHT];
    binding->upload_width = (u16)upload_width;
    binding->upload_height = (u16)upload_height;
    binding->tex = gx_upload_texture((const GXTexObj*)binding->object_words,
                                     &binding->width, &binding->height,
                                     &binding->upload_width,
                                     &binding->upload_height);
    binding->format = o[TEXOBJ_FORMAT];
    binding->refreshed_frame = s_tex_cache_frame;
    if (s_active_texmap == map) gx_select_texmap(map);
}

static void gx_dl_write(const void* data, u32 len) {
    if (!s_dl_active || s_dl_overflow) return;
    if (s_dl_offset + len > s_dl_size) {
        s_dl_overflow = 1;
        return;
    }
    memcpy(s_dl_buffer + s_dl_offset, data, len);
    s_dl_offset += len;
}

static u32 gx_pos_mtx_slot(u32 id) {
    u32 slot = id / 3;
    return slot < 10 ? slot : 0;
}

static int gx_tex_mtx_slot(u32 id) {
    if (id == GX_IDENTITY) return -1;
    if (id < 10) return (int)id;
    if (id >= GX_TEXMTX0 && id < GX_IDENTITY) return (int)(id - GX_TEXMTX0) / 3;
    return -1;
}


static void gx_commit(void) {
    if (s_pending && s_count < ACGC_GX_MAX_INPUT) s_vertices[s_count++] = s_current;
    s_pending = 0;
}

static void gx_draw(void);

static void gx_flush_if_complete(void) {
    if (s_expected_count != 0 && (u16)(s_count + (s_pending ? 1 : 0)) >= s_expected_count) {
        gx_draw();
    }
}

static void gx_finish_attribute(GXAttr attr) {
    for (int next = (int)attr + 1; next <= GX_VA_TEX7; next++) {
        if (s_vtx_desc[next] != GX_NONE) return;
    }
    gx_flush_if_complete();
}

static void gx_build_modelview(f32 out[4][4]) {
    u32 mtx_slot = s_current_mtx < 10 ? s_current_mtx : 0;
    const f32 (*m)[4] = s_position[mtx_slot];

    memset(out, 0, sizeof(f32) * 16);
    out[0][0] = m[0][0]; out[0][1] = m[0][1]; out[0][2] = m[0][2]; out[0][3] = m[0][3];
    out[1][0] = m[1][0]; out[1][1] = m[1][1]; out[1][2] = m[1][2]; out[1][3] = m[1][3];
    out[2][0] = m[2][0]; out[2][1] = m[2][1]; out[2][2] = m[2][2]; out[2][3] = m[2][3];
    out[3][3] = 1.0f;
}

static Acgc3dsGpuVertex gx_raw_vertex(const AcgcGxVertex* in, const f32 face_normal[3]) {
    Acgc3dsGpuVertex out;
    f32 source_nx = in->nx;
    f32 source_ny = in->ny;
    f32 source_nz = in->nz;
    f32 s = in->s;
    f32 t = in->t;
    f32 s1 = in->s;
    f32 t1 = in->t;
    int tex_mtx_slot = s_active_texcoord < GX_MAX_TEXCOORD ?
                       gx_tex_mtx_slot(s_texgen_mtx[s_active_texcoord]) : -1;
    if (tex_mtx_slot >= 0) {
        const f32 (*m)[4] = s_texture_mtx[tex_mtx_slot];
        s = m[0][0] * in->s + m[0][1] * in->t + m[0][3];
        t = m[1][0] * in->s + m[1][1] * in->t + m[1][3];
    }
    tex_mtx_slot = gx_tex_mtx_slot(s_texgen_mtx[GX_TEXCOORD1]);
    if (tex_mtx_slot >= 0) {
        const f32 (*m)[4] = s_texture_mtx[tex_mtx_slot];
        s1 = m[0][0] * in->s + m[0][1] * in->t + m[0][3];
        t1 = m[1][0] * in->s + m[1][1] * in->t + m[1][3];
    }
    f32 pad_s = s_bound_texture_upload_width != 0 ?
                (f32)s_bound_texture_width / (f32)s_bound_texture_upload_width : 1.0f;
    f32 pad_t = s_bound_texture_upload_height != 0 ?
                (f32)s_bound_texture_height / (f32)s_bound_texture_upload_height : 1.0f;
    f32 ras_r;
    f32 ras_g;
    f32 ras_b;
    f32 ras_a;

    /* GX assets occasionally contain zeroed or inward-facing normals. Keep
     * smooth vertex normals, but force them into the hemisphere selected by
     * the triangle's front-face winding. */
    {
        f32 source_len_sq = source_nx * source_nx + source_ny * source_ny +
                            source_nz * source_nz;
        f32 face_len_sq = face_normal[0] * face_normal[0] +
                          face_normal[1] * face_normal[1] +
                          face_normal[2] * face_normal[2];
        if (face_len_sq > 0.0000000001f) {
            if (source_len_sq <= 0.0000000001f) {
                source_nx = face_normal[0];
                source_ny = face_normal[1];
                source_nz = face_normal[2];
            } else if (source_nx * face_normal[0] +
                       source_ny * face_normal[1] +
                       source_nz * face_normal[2] < 0.0f) {
                source_nx = -source_nx;
                source_ny = -source_ny;
                source_nz = -source_nz;
            }
        }
    }

    if (s_num_chans == 0) {
        ras_r = ras_g = ras_b = ras_a = 1.0f;
    } else {
        f32 mat_r = s_color0_channel.mat_src == GX_SRC_VTX ? in->r / 255.0f : s_channel_material.r / 255.0f;
        f32 mat_g = s_color0_channel.mat_src == GX_SRC_VTX ? in->g / 255.0f : s_channel_material.g / 255.0f;
        f32 mat_b = s_color0_channel.mat_src == GX_SRC_VTX ? in->b / 255.0f : s_channel_material.b / 255.0f;
        f32 amb_r = s_color0_channel.amb_src == GX_SRC_VTX ? in->r / 255.0f : s_channel_ambient.r / 255.0f;
        f32 amb_g = s_color0_channel.amb_src == GX_SRC_VTX ? in->g / 255.0f : s_channel_ambient.g / 255.0f;
        f32 amb_b = s_color0_channel.amb_src == GX_SRC_VTX ? in->b / 255.0f : s_channel_ambient.b / 255.0f;
        f32 mat_a = s_alpha0_channel.mat_src == GX_SRC_VTX ? in->a / 255.0f : s_channel_material.a / 255.0f;

        if (s_color0_channel.enable) {
            const f32 (*nrm)[3] = s_normal[s_current_mtx < 10 ? s_current_mtx : 0];
            f32 tx = nrm[0][0] * source_nx + nrm[0][1] * source_ny + nrm[0][2] * source_nz;
            f32 ty = nrm[1][0] * source_nx + nrm[1][1] * source_ny + nrm[1][2] * source_nz;
            f32 tz = nrm[2][0] * source_nx + nrm[2][1] * source_ny + nrm[2][2] * source_nz;
            f32 n_len = sqrtf(tx * tx + ty * ty + tz * tz);
            f32 nx = n_len > 0.00001f ? tx / n_len : 0.0f;
            f32 ny = n_len > 0.00001f ? ty / n_len : 0.0f;
            f32 nz = n_len > 0.00001f ? tz / n_len : 0.0f;
            for (int i = 0; i < 8; i++) {
                if ((s_color0_channel.light_mask & (1u << i)) != 0) {
                    f32 l_len = sqrtf(s_lights[i].px * s_lights[i].px +
                                      s_lights[i].py * s_lights[i].py +
                                      s_lights[i].pz * s_lights[i].pz);
                    if (l_len > 0.00001f) {
                        f32 dot = nx * (s_lights[i].px / l_len) +
                                  ny * (s_lights[i].py / l_len) +
                                  nz * (s_lights[i].pz / l_len);
                        if (dot < 0.0f) dot = 0.0f;
                        if (dot > 1.0f) dot = 1.0f;
                        amb_r += dot * s_lights[i].r;
                        amb_g += dot * s_lights[i].g;
                        amb_b += dot * s_lights[i].b;
                    }
                }
            }
            if (amb_r > 1.0f) amb_r = 1.0f;
            if (amb_g > 1.0f) amb_g = 1.0f;
            if (amb_b > 1.0f) amb_b = 1.0f;
            ras_r = mat_r * amb_r;
            ras_g = mat_g * amb_g;
            ras_b = mat_b * amb_b;
        } else {
            ras_r = mat_r;
            ras_g = mat_g;
            ras_b = mat_b;
        }
        ras_a = s_alpha0_channel.enable ? mat_a * (s_channel_ambient.a / 255.0f) : mat_a;
    }
    out.x = in->x;
    out.y = in->y;
    out.z = in->z;
    out.s = s * pad_s;
    out.t = 1.0f - t * pad_t;
    out.s1 = s1 * (s_tex_bindings[GX_TEXMAP1].upload_width != 0 ?
                    (f32)s_tex_bindings[GX_TEXMAP1].width /
                    (f32)s_tex_bindings[GX_TEXMAP1].upload_width : 1.0f);
    out.t1 = 1.0f - t1 * (s_tex_bindings[GX_TEXMAP1].upload_height != 0 ?
                          (f32)s_tex_bindings[GX_TEXMAP1].height /
                          (f32)s_tex_bindings[GX_TEXMAP1].upload_height : 1.0f);
    out.r = ras_r;
    out.g = ras_g;
    out.b = ras_b;
    out.a = ras_a;
    return out;
}

static void gx_emit_triangle(u16 a, u16 b, u16 c, size_t* count) {
    const AcgcGxVertex* va;
    const AcgcGxVertex* vb;
    const AcgcGxVertex* vc;
    f32 abx;
    f32 aby;
    f32 abz;
    f32 acx;
    f32 acy;
    f32 acz;
    f32 face_normal[3];

    if (*count + 3 > ACGC_GX_MAX_OUTPUT) return;
    va = &s_vertices[a];
    vb = &s_vertices[b];
    vc = &s_vertices[c];
    abx = vb->x - va->x;
    aby = vb->y - va->y;
    abz = vb->z - va->z;
    acx = vc->x - va->x;
    acy = vc->y - va->y;
    acz = vc->z - va->z;
    face_normal[0] = aby * acz - abz * acy;
    face_normal[1] = abz * acx - abx * acz;
    face_normal[2] = abx * acy - aby * acx;

    s_output[(*count)++] = gx_raw_vertex(va, face_normal);
    s_output[(*count)++] = gx_raw_vertex(vb, face_normal);
    s_output[(*count)++] = gx_raw_vertex(vc, face_normal);
}

static void gx_draw(void) {
    size_t out_count = 0;
    f32 modelview[4][4];
    Acgc3dsTevPreset preset;
    GXTexMapID font_map;

    /* Resolve and refresh texture state before classifying the TEV program.
     * Font commands are commonly represented as a two-stage translation of
     * the N64 combiner, so the last selected map is not a reliable indication
     * of which texture provides glyph coverage. */
    gx_resolve_tev_texture();
    gx_refresh_tev_bindings();
    preset = gx_tev_preset();
    font_map = preset == ACGC_3DS_TEV_FONT_MASK ?
               gx_find_tev_i4_texmap() : GX_TEXMAP_NULL;
    if (font_map != GX_TEXMAP_NULL) {
        /* gx_raw_vertex uses the selected binding to compensate for the
         * power-of-two padding required by PICA200 textures. */
        gx_select_texmap(font_map);
    }
    if (preset == ACGC_3DS_TEV_SPOTLIGHT || preset == ACGC_3DS_TEV_MASKED) {
        gx_refresh_binding(GX_TEXMAP0);
        gx_refresh_binding(GX_TEXMAP1);
    }
    gx_commit();
    switch (s_primitive) {
        case GX_TRIANGLES:
            for (u16 i = 0; i + 2 < s_count; i += 3) gx_emit_triangle(i, i + 1, i + 2, &out_count);
            break;
        case GX_TRIANGLESTRIP:
            for (u16 i = 0; i + 2 < s_count; ++i) {
                if (i & 1) gx_emit_triangle(i + 1, i, i + 2, &out_count);
                else gx_emit_triangle(i, i + 1, i + 2, &out_count);
            }
            break;
        case GX_TRIANGLEFAN:
            for (u16 i = 1; i + 1 < s_count; ++i) gx_emit_triangle(0, i, i + 1, &out_count);
            break;
        case GX_QUADS:
            for (u16 i = 0; i + 3 < s_count; i += 4) {
                gx_emit_triangle(i, i + 1, i + 2, &out_count);
                gx_emit_triangle(i, i + 2, i + 3, &out_count);
            }
            break;
        default:
            break;
    }
    if (out_count) {
        int draw_depth_test = s_depth_test;
        int draw_depth_write = s_depth_write;
        int alpha_test;
        int alpha_func;
        int alpha_ref;

        /* Font is the final UI layer and must neither test nor write scene
         * depth. Other draws retain the GX depth-write state: decal overlays
         * rely on the game's explicit multipass ordering. */
        if (preset == ACGC_3DS_TEV_FONT_MASK) {
            draw_depth_test = GX_FALSE;
            draw_depth_write = GX_FALSE;
        }

        gx_build_modelview(modelview);
        gx_resolve_alpha_test(&alpha_test, &alpha_func, &alpha_ref);
        C3D_Tex* texture0 = (preset == ACGC_3DS_TEV_SPOTLIGHT || preset == ACGC_3DS_TEV_MASKED) ?
                            s_tex_bindings[GX_TEXMAP0].tex :
                            (font_map != GX_TEXMAP_NULL ? s_tex_bindings[font_map].tex :
                                                         s_bound_texture);
        C3D_Tex* texture1 = (preset == ACGC_3DS_TEV_SPOTLIGHT || preset == ACGC_3DS_TEV_MASKED) ?
                            s_tex_bindings[GX_TEXMAP1].tex : NULL;
        u32 color1 = ((u32)s_tev_colors[GX_TEVREG1].a << 24) |
                     ((u32)s_tev_colors[GX_TEVREG1].b << 16) |
                     ((u32)s_tev_colors[GX_TEVREG1].g << 8) |
                     s_tev_colors[GX_TEVREG1].r;
        u32 color2 = ((u32)s_tev_colors[GX_TEVREG2].a << 24) |
                     ((u32)s_tev_colors[GX_TEVREG2].b << 16) |
                     ((u32)s_tev_colors[GX_TEVREG2].g << 8) |
                     s_tev_colors[GX_TEVREG2].r;
        acgc_3ds_video_draw_gx_triangles(s_output, out_count, s_projection, modelview,
                                         texture0, texture1, color1, color2, preset,
                                         s_render_screen ? ACGC_3DS_RENDER_UI
                                                         : ACGC_3DS_RENDER_WORLD,
                                         s_viewport,
                                         s_blend_mode, s_blend_src, s_blend_dst,
                                         alpha_test, alpha_func, alpha_ref,
                                         draw_depth_test, s_depth_func,
                                         draw_depth_write,
                                         s_color_update, s_alpha_update);
    }
    s_count = 0;
    s_expected_count = 0;
}

static int gx_skip_attr(const u8** p, const u8* end, GXAttr attr, GXVtxFmt fmt) {
    GXAttrType desc = s_vtx_desc[attr];
    GXCompCnt cnt = s_vtx_cnt[fmt][attr];
    GXCompType type = s_vtx_type[fmt][attr];
    u32 bytes = 0;

    if (desc == GX_NONE) return 1;
    if (desc == GX_INDEX8) bytes = 1;
    else if (desc == GX_INDEX16) bytes = 2;
    else if (desc == GX_DIRECT) {
        switch (attr) {
            case GX_VA_PNMTXIDX:
            case GX_VA_TEX0MTXIDX:
            case GX_VA_TEX1MTXIDX:
            case GX_VA_TEX2MTXIDX:
            case GX_VA_TEX3MTXIDX:
            case GX_VA_TEX4MTXIDX:
            case GX_VA_TEX5MTXIDX:
            case GX_VA_TEX6MTXIDX:
            case GX_VA_TEX7MTXIDX:
                bytes = 1;
                break;
            case GX_VA_NRM:
                bytes = 3 * (type == GX_F32 ? 4 : (type == GX_S16 || type == GX_U16 ? 2 : 1));
                break;
            case GX_VA_CLR0:
            case GX_VA_CLR1:
                bytes = (type == GX_RGB565 || type == GX_RGBA4) ? 2 : (type == GX_RGBA6 ? 3 : 4);
                break;
            case GX_VA_TEX0:
            case GX_VA_TEX1:
            case GX_VA_TEX2:
            case GX_VA_TEX3:
            case GX_VA_TEX4:
            case GX_VA_TEX5:
            case GX_VA_TEX6:
            case GX_VA_TEX7:
                bytes = (cnt == GX_TEX_ST ? 2 : 1) *
                        (type == GX_F32 ? 4 : (type == GX_S16 || type == GX_U16 ? 2 : 1));
                break;
            default:
                break;
        }
    }

    if (*p + bytes > end) return 0;
    *p += bytes;
    return 1;
}

static int gx_read_direct_pos(const u8** p, const u8* end, GXVtxFmt fmt) {
    GXCompCnt cnt = s_vtx_cnt[fmt][GX_VA_POS];
    GXCompType type = s_vtx_type[fmt][GX_VA_POS];
    u32 bytes = cnt == GX_POS_XYZ ? 12 : 8;

    if (s_vtx_desc[GX_VA_POS] == GX_NONE) return 1;
    if (s_vtx_desc[GX_VA_POS] == GX_INDEX8 || s_vtx_desc[GX_VA_POS] == GX_INDEX16) {
        u16 index;
        const u8* src;
        if (*p + (s_vtx_desc[GX_VA_POS] == GX_INDEX16 ? 2 : 1) > end) return 0;
        index = s_vtx_desc[GX_VA_POS] == GX_INDEX16 ? gx_read_be16(*p) : **p;
        *p += s_vtx_desc[GX_VA_POS] == GX_INDEX16 ? 2 : 1;
        if (s_array_base[GX_VA_POS] == NULL || type != GX_F32) return 1;
        src = s_array_base[GX_VA_POS] + index * s_array_stride[GX_VA_POS];
        GXPosition3f32(((const f32*)src)[0], ((const f32*)src)[1],
                       cnt == GX_POS_XYZ ? ((const f32*)src)[2] : 0.0f);
        return 1;
    }
    if (s_vtx_desc[GX_VA_POS] != GX_DIRECT || type != GX_F32) return gx_skip_attr(p, end, GX_VA_POS, fmt);
    if (*p + bytes > end) return 0;
    GXPosition3f32(gx_read_be_f32(*p), gx_read_be_f32(*p + 4),
                   cnt == GX_POS_XYZ ? gx_read_be_f32(*p + 8) : 0.0f);
    *p += bytes;
    return 1;
}

static int gx_read_direct_color(const u8** p, const u8* end, GXAttr attr, GXVtxFmt fmt) {
    GXCompType type = s_vtx_type[fmt][attr];

    if (s_vtx_desc[attr] == GX_NONE) return 1;
    if (s_vtx_desc[attr] == GX_INDEX8 || s_vtx_desc[attr] == GX_INDEX16) {
        u16 index;
        const u8* clr;
        if (*p + (s_vtx_desc[attr] == GX_INDEX16 ? 2 : 1) > end) return 0;
        index = s_vtx_desc[attr] == GX_INDEX16 ? gx_read_be16(*p) : **p;
        *p += s_vtx_desc[attr] == GX_INDEX16 ? 2 : 1;
        if (s_array_base[attr] == NULL) return 1;
        clr = s_array_base[attr] + index * s_array_stride[attr];
        GXColor4u8(clr[0], clr[1], clr[2], type == GX_RGB8 ? 255 : clr[3]);
        return 1;
    }
    if (s_vtx_desc[attr] != GX_DIRECT) return gx_skip_attr(p, end, attr, fmt);
    if (type == GX_RGBA8 || type == GX_RGBX8 || type == GX_RGB8) {
        if (*p + 4 > end) return 0;
        GXColor4u8((*p)[0], (*p)[1], (*p)[2], type == GX_RGB8 ? 255 : (*p)[3]);
        *p += 4;
        return 1;
    }
    return gx_skip_attr(p, end, attr, fmt);
}

static int gx_read_direct_texcoord(const u8** p, const u8* end, GXAttr attr, GXVtxFmt fmt) {
    GXCompCnt cnt = s_vtx_cnt[fmt][attr];
    GXCompType type = s_vtx_type[fmt][attr];
    f32 s = 0.0f;
    f32 t = 0.0f;

    if (s_vtx_desc[attr] == GX_NONE) return 1;
    if (s_vtx_desc[attr] == GX_INDEX8 || s_vtx_desc[attr] == GX_INDEX16) {
        u16 index;
        const u8* src;
        if (*p + (s_vtx_desc[attr] == GX_INDEX16 ? 2 : 1) > end) return 0;
        index = s_vtx_desc[attr] == GX_INDEX16 ? gx_read_be16(*p) : **p;
        *p += s_vtx_desc[attr] == GX_INDEX16 ? 2 : 1;
        if (s_array_base[attr] == NULL) return 1;
        src = s_array_base[attr] + index * s_array_stride[attr];
        if (type == GX_F32) {
            s = ((const f32*)src)[0];
            t = cnt == GX_TEX_ST ? ((const f32*)src)[1] : 0.0f;
            GXTexCoord2f32(s, t);
        } else if (type == GX_S16) {
            GXTexCoord2s16(((const s16*)src)[0], cnt == GX_TEX_ST ? ((const s16*)src)[1] : 0);
        } else if (type == GX_U16) {
            GXTexCoord2s16((s16)((const u16*)src)[0], cnt == GX_TEX_ST ? (s16)((const u16*)src)[1] : 0);
        } else {
            GXTexCoord2u8(src[0], cnt == GX_TEX_ST ? src[1] : 0);
        }
        return 1;
    }
    if (s_vtx_desc[attr] != GX_DIRECT) return gx_skip_attr(p, end, attr, fmt);
    if (type == GX_F32) {
        if (*p + (cnt == GX_TEX_ST ? 8 : 4) > end) return 0;
        s = gx_read_be_f32(*p);
        t = cnt == GX_TEX_ST ? gx_read_be_f32(*p + 4) : 0.0f;
        *p += cnt == GX_TEX_ST ? 8 : 4;
        GXTexCoord2f32(s, t);
    } else if (type == GX_S16 || type == GX_U16) {
        if (*p + (cnt == GX_TEX_ST ? 4 : 2) > end) return 0;
        s = type == GX_S16 ? (f32)(s16)gx_read_be16(*p) : (f32)gx_read_be16(*p);
        t = cnt == GX_TEX_ST ? (type == GX_S16 ? (f32)(s16)gx_read_be16(*p + 2) : (f32)gx_read_be16(*p + 2)) : 0.0f;
        *p += cnt == GX_TEX_ST ? 4 : 2;
        GXTexCoord2s16((s16)s, (s16)t);
    } else {
        if (*p + (cnt == GX_TEX_ST ? 2 : 1) > end) return 0;
        s = (*p)[0];
        t = cnt == GX_TEX_ST ? (*p)[1] : 0.0f;
        *p += cnt == GX_TEX_ST ? 2 : 1;
        GXTexCoord2u8((u8)s, (u8)t);
    }
    return 1;
}

static int gx_replay_raw_display_list(const void* list, u32 nbytes) {
    const u8* p = (const u8*)list;
    const u8* end = p + nbytes;
    int drew = 0;

    while (p < end) {
        u8 op = *p++;
        if (op == GX_NOP) continue;
        if (op == GX_LOAD_CP_REG) {
            u8 reg;
            u32 value;
            if (p + 5 > end) return drew;
            reg = *p++;
            value = gx_read_be32(p);
            p += 4;
            if (reg == 0x50) {
                s_vtx_desc[GX_VA_POS] = (GXAttrType)((value >> 9) & 3);
                s_vtx_desc[GX_VA_NRM] = (GXAttrType)((value >> 11) & 3);
                s_vtx_desc[GX_VA_CLR0] = (GXAttrType)((value >> 13) & 3);
                s_vtx_desc[GX_VA_CLR1] = (GXAttrType)((value >> 15) & 3);
            } else if (reg == 0x60) {
                s_vtx_desc[GX_VA_TEX0] = (GXAttrType)((value >> 0) & 3);
                s_vtx_desc[GX_VA_TEX1] = (GXAttrType)((value >> 2) & 3);
                s_vtx_desc[GX_VA_TEX2] = (GXAttrType)((value >> 4) & 3);
                s_vtx_desc[GX_VA_TEX3] = (GXAttrType)((value >> 6) & 3);
                s_vtx_desc[GX_VA_TEX4] = (GXAttrType)((value >> 8) & 3);
                s_vtx_desc[GX_VA_TEX5] = (GXAttrType)((value >> 10) & 3);
                s_vtx_desc[GX_VA_TEX6] = (GXAttrType)((value >> 12) & 3);
                s_vtx_desc[GX_VA_TEX7] = (GXAttrType)((value >> 14) & 3);
            }
            continue;
        }
        if ((op & GX_OPCODE_MASK) >= GX_DRAW_QUADS && (op & GX_OPCODE_MASK) <= GX_DRAW_POINTS) {
            GXPrimitive prim = (GXPrimitive)(op & GX_OPCODE_MASK);
            GXVtxFmt fmt = (GXVtxFmt)(op & GX_VAT_MASK);
            u16 count;
            if (p + 2 > end) return drew;
            count = gx_read_be16(p);
            p += 2;
            GXBegin(prim, fmt, count);
            for (u16 i = 0; i < count; ++i) {
                for (GXAttr attr = GX_VA_PNMTXIDX; attr < GX_VA_POS; attr++) {
                    if (!gx_skip_attr(&p, end, attr, fmt)) return drew;
                }
                if (!gx_read_direct_pos(&p, end, fmt)) return drew;
                if (!gx_skip_attr(&p, end, GX_VA_NRM, fmt)) return drew;
                if (!gx_read_direct_color(&p, end, GX_VA_CLR0, fmt)) return drew;
                if (!gx_read_direct_color(&p, end, GX_VA_CLR1, fmt)) return drew;
                if (!gx_read_direct_texcoord(&p, end, GX_VA_TEX0, fmt)) return drew;
                for (GXAttr attr = GX_VA_TEX1; attr <= GX_VA_TEX7; attr++) {
                    if (!gx_skip_attr(&p, end, attr, fmt)) return drew;
                }
            }
            GXEnd();
            drew = 1;
            continue;
        }
        break;
    }
    return drew;
}

void pc_gx_begin_frame(void) {
    s_render_screen = 0;
    s_tex_cache_frame++;
    if (s_tex_cache_frame == 0) {
        s_tex_cache_frame = 1;
        for (int i = 0; i < ACGC_3DS_TEX_CACHE_SIZE; i++) {
            s_tex_cache[i].last_used_frame = 0;
        }
    }
}
void pc_gx_draw_pending(void) { gx_draw(); }
void pc_gx_set_render_screen(int bottom_screen) {
    gx_draw();
    s_render_screen = bottom_screen != 0;
}
void pc_gx_tlut_set_native_le(int idx) {
    if ((unsigned)idx < 16) s_tluts[idx].is_be = 0;
}

GXFifoObj* GXInit(void* base, u32 size) { (void)base; (void)size; gx_identity(); gx_state_defaults(); return &s_fifo; }
void GXBegin(GXPrimitive primitive, GXVtxFmt fmt, u16 nverts) {
    (void)fmt; gx_draw(); s_primitive = primitive; s_count = 0; s_pending = 0; s_expected_count = nverts;
}
void GXEnd(void) { gx_draw(); }
void GXPosition3f32(f32 x, f32 y, f32 z) {
    u8 r = s_current.r;
    u8 g = s_current.g;
    u8 b = s_current.b;
    u8 a = s_current.a;
    gx_commit(); memset(&s_current, 0, sizeof(s_current));
    s_current.x = x; s_current.y = y; s_current.z = z;
    s_current.r = r;
    s_current.g = g;
    s_current.b = b;
    s_current.a = a;
    s_pending = 1;
    gx_finish_attribute(GX_VA_POS);
}
void GXPosition2f32(f32 x, f32 y) { GXPosition3f32(x, y, 0); }
void GXPosition2u16(u16 x, u16 y) { GXPosition3f32(x, y, 0); }
void GXNormal3f32(f32 x, f32 y, f32 z) { s_current.nx=x; s_current.ny=y; s_current.nz=z; gx_finish_attribute(GX_VA_NRM); }
void GXColor4u8(u8 r, u8 g, u8 b, u8 a) { s_current.r=r; s_current.g=g; s_current.b=b; s_current.a=a; gx_finish_attribute(GX_VA_CLR0); }
void GXColor1u32(u32 c) { GXColor4u8(c >> 24, c >> 16, c >> 8, c); }
void GXTexCoord2f32(f32 s, f32 t) { s_current.s=s; s_current.t=t; gx_finish_attribute(GX_VA_TEX0); }
void GXTexCoord2s16(s16 s, s16 t) { s_current.s=s; s_current.t=t; gx_finish_attribute(GX_VA_TEX0); }
void GXTexCoord2u8(u8 s, u8 t) { s_current.s=s; s_current.t=t; gx_finish_attribute(GX_VA_TEX0); }

void GXSetProjection(const void* mtx, GXProjectionType type) {
    memcpy(s_projection, mtx, sizeof(f32) * 12);
    if (type == GX_PERSPECTIVE) {
        s_projection[3][0] = 0.0f;
        s_projection[3][1] = 0.0f;
        s_projection[3][2] = -1.0f;
        s_projection[3][3] = 0.0f;
    } else {
        s_projection[3][0] = 0.0f;
        s_projection[3][1] = 0.0f;
        s_projection[3][2] = 0.0f;
        s_projection[3][3] = 1.0f;
    }
}
void GXLoadPosMtxImm(const void* mtx, u32 id) { u32 slot=gx_pos_mtx_slot(id); memcpy(s_position[slot], mtx, sizeof(s_position[slot])); }
void GXLoadNrmMtxImm(const void* mtx, u32 id) {
    const f32 (*src)[4] = (const f32 (*)[4])mtx;
    u32 slot = gx_pos_mtx_slot(id);
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) s_normal[slot][row][col] = src[row][col];
    }
}
void GXLoadTexMtxImm(const void* mtx, u32 id, GXTexMtxType type) {
    int slot = gx_tex_mtx_slot(id);
    (void)type;
    if (slot >= 0) memcpy(s_texture_mtx[slot], mtx, sizeof(s_texture_mtx[slot]));
}
void GXSetCurrentMtx(u32 id) { s_current_mtx = gx_pos_mtx_slot(id); }
void GXSetViewport(f32 l,f32 t,f32 w,f32 h,f32 n,f32 f) {
    gx_draw();
    s_viewport[0]=l;s_viewport[1]=t;s_viewport[2]=w;s_viewport[3]=h;s_viewport[4]=n;s_viewport[5]=f;
}

#define IGNORE1(name,t1) void name(t1 a){(void)a;}
#define IGNORE2(name,t1,t2) void name(t1 a,t2 b){(void)a;(void)b;}
#define IGNORE3(name,t1,t2,t3) void name(t1 a,t2 b,t3 c){(void)a;(void)b;(void)c;}
#define IGNORE4(name,t1,t2,t3,t4) void name(t1 a,t2 b,t3 c,t4 d){(void)a;(void)b;(void)c;(void)d;}
#define IGNORE5(name,t1,t2,t3,t4,t5) void name(t1 a,t2 b,t3 c,t4 d,t5 e){(void)a;(void)b;(void)c;(void)d;(void)e;}
#define IGNORE6(name,t1,t2,t3,t4,t5,t6) void name(t1 a,t2 b,t3 c,t4 d,t5 e,t6 f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;}

void GXAbortFrame(void) { s_count=s_pending=0; }
void GXFlush(void) { gx_draw(); }
void GXDrawDone(void) { gx_draw(); }
void GXSetDrawDone(void) { gx_draw(); if (s_draw_done_callback) s_draw_done_callback(); }
GXDrawDoneCallback GXSetDrawDoneCallback(GXDrawDoneCallback cb) { GXDrawDoneCallback old=s_draw_done_callback;s_draw_done_callback=cb;return old; }
void GXPixModeSync(void) {}
void GXInvalidateTexAll(void) {
    /* Content hashes are revalidated at draw time. Keeping allocations alive
     * here is required because current-frame C3D commands may reference them. */
}
void GXInvalidateVtxCache(void) {}
void GXClearVtxDesc(void) { memset(s_vtx_desc, 0, sizeof(s_vtx_desc)); }
void GXBeginDisplayList(void* list,u32 size){s_dl_buffer=(u8*)list;s_dl_size=size;s_dl_offset=0;s_dl_active=1;s_dl_overflow=0;}
u32 GXEndDisplayList(void){u32 n=s_dl_active&&!s_dl_overflow?s_dl_offset:0;s_dl_buffer=NULL;s_dl_size=0;s_dl_offset=0;s_dl_active=0;s_dl_overflow=0;return n;}
void GXCallDisplayList(void* list,u32 size){
    if (list == NULL || size == 0) return;
    if (gx_replay_raw_display_list(list, size)) {
        extern int pc_gx_draw_call_count;
        pc_gx_draw_call_count++;
    }
}
void GXInitFifoBase(GXFifoObj* f,void* b,u32 s){(void)f;(void)b;(void)s;}
void GXInitFifoPtrs(GXFifoObj* f,void* r,void* w){(void)f;(void)r;(void)w;}
void GXSaveCPUFifo(GXFifoObj* f){(void)f;}
OSThread* GXSetCurrentGXThread(void){return NULL;}
void GXGetGPStatus(GXBool*a,GXBool*b,GXBool*c,GXBool*d,GXBool*e){if(a)*a=0;if(b)*b=0;if(c)*c=1;if(d)*d=1;if(e)*e=0;}
void GXReadXfRasMetric(u32*a,u32*b,u32*c,u32*d){if(a)*a=0;if(b)*b=0;if(c)*c=0;if(d)*d=0;}

void GXSetVtxDesc(GXAttr a,GXAttrType b){ if (a < GX_VA_MAX_ATTR) s_vtx_desc[a]=b; }
void GXSetVtxAttrFmt(GXVtxFmt a,GXAttr b,GXCompCnt c,GXCompType d,u8 e){if(a<GX_MAX_VTXFMT&&b<GX_VA_MAX_ATTR){s_vtx_cnt[a][b]=c;s_vtx_type[a][b]=d;s_vtx_frac[a][b]=e;}}
void GXSetArray(GXAttr a,const void* b,u32 c,u8 d){(void)c;if(a<GX_VA_MAX_ATTR){s_array_base[a]=(const u8*)b;s_array_stride[a]=d;}}
void GXSetTexCoordGen2(GXTexCoordID a,GXTexGenType b,GXTexGenSrc c,u32 d,GXBool e,u32 f){
    (void)b;(void)c;(void)e;(void)f;
    if (a < GX_MAX_TEXCOORD) s_texgen_mtx[a] = d;
}
IGNORE2(GXSetLineWidth,u8,GXTexOffset)
IGNORE2(GXSetPointSize,u8,GXTexOffset)
IGNORE3(GXEnableTexOffsets,GXTexCoordID,GXBool,GXBool)
IGNORE1(GXSetNumTexGens,u8)
void GXSetNumChans(u8 count) { gx_draw(); s_num_chans = count; }
IGNORE1(GXSetNumIndStages,u8)
void GXSetNumTevStages(u8 count){
    gx_draw();
    s_num_tev_stages = count > GX_MAX_TEVSTAGE ? GX_MAX_TEVSTAGE : count;
    gx_resolve_tev_texture();
}
IGNORE1(GXSetCullMode,GXCullMode)
IGNORE1(GXSetCoPlanar,GXBool)
IGNORE1(GXSetClipMode,GXClipMode)
IGNORE2(GXSetScissorBoxOffset,s32,s32)
void GXSetScissor(u32 a,u32 b,u32 c,u32 d){(void)a;(void)b;(void)c;(void)d;}
void GXSetChanCtrl(GXChannelID chan,GXBool enable,GXColorSrc amb_src,GXColorSrc mat_src,u32 light_mask,GXDiffuseFn diff_fn,GXAttnFn attn_fn){
    Acgc3dsChannelState state;
    gx_draw();
    state.enable = enable;
    state.amb_src = amb_src;
    state.mat_src = mat_src;
    state.light_mask = light_mask;
    state.diff_fn = diff_fn;
    state.attn_fn = attn_fn;
    if (chan == GX_COLOR0 || chan == GX_COLOR0A0) s_color0_channel = state;
    if (chan == GX_ALPHA0 || chan == GX_COLOR0A0) s_alpha0_channel = state;
}
void GXSetChanAmbColor(GXChannelID chan,GXColor color){
    gx_draw();
    if (chan == GX_COLOR0 || chan == GX_ALPHA0 || chan == GX_COLOR0A0) s_channel_ambient = color;
}
void GXSetChanMatColor(GXChannelID chan,GXColor color){
    gx_draw();
    if (chan == GX_COLOR0 || chan == GX_ALPHA0 || chan == GX_COLOR0A0) s_channel_material = color;
}
void GXInitLightAttn(GXLightObj*obj,f32 a0,f32 a1,f32 a2,f32 k0,f32 k1,f32 k2){
    Acgc3dsLightObjInternal* light = (Acgc3dsLightObjInternal*)obj;
    light->a0=a0;light->a1=a1;light->a2=a2;light->k0=k0;light->k1=k1;light->k2=k2;
}
void GXInitLightPos(GXLightObj*obj,f32 x,f32 y,f32 z){
    Acgc3dsLightObjInternal* light = (Acgc3dsLightObjInternal*)obj;
    light->px=x;light->py=y;light->pz=z;
}
void GXInitLightDir(GXLightObj*obj,f32 x,f32 y,f32 z){
    Acgc3dsLightObjInternal* light = (Acgc3dsLightObjInternal*)obj;
    light->nx=x;light->ny=y;light->nz=z;
}
void GXInitLightColor(GXLightObj*obj,GXColor color){
    Acgc3dsLightObjInternal* light = (Acgc3dsLightObjInternal*)obj;
    memcpy(&light->color, &color, sizeof(color));
}
void GXLoadLightObjImm(GXLightObj*obj,GXLightID id){
    Acgc3dsLightObjInternal* light = (Acgc3dsLightObjInternal*)obj;
    GXColor color;
    int slot = -1;
    for (int i = 0; i < 8; i++) if ((u32)id == (1u << i)) { slot = i; break; }
    if (slot < 0) return;
    gx_draw();
    memcpy(&color, &light->color, sizeof(color));
    s_lights[slot].px = light->px;
    s_lights[slot].py = light->py;
    s_lights[slot].pz = light->pz;
    s_lights[slot].r = color.r / 255.0f;
    s_lights[slot].g = color.g / 255.0f;
    s_lights[slot].b = color.b / 255.0f;
    s_lights[slot].a = color.a / 255.0f;
}

void GXInitTexObj(GXTexObj*a,void*b,u16 c,u16 d,GXTexFmt e,GXTexWrapMode f,GXTexWrapMode g,u8 h){
    u32* o = (u32*)a;
    memset(o, 0, TEXOBJ_SIZE * sizeof(u32));
    o[TEXOBJ_IMAGE_PTR] = (u32)(uintptr_t)b;
    o[TEXOBJ_WIDTH] = c;
    o[TEXOBJ_HEIGHT] = d;
    o[TEXOBJ_FORMAT] = e;
    o[TEXOBJ_WRAP_S] = f;
    o[TEXOBJ_WRAP_T] = g;
    o[TEXOBJ_MIPMAP] = h;
    o[TEXOBJ_MIN_FILTER] = GX_LINEAR;
    o[TEXOBJ_MAG_FILTER] = GX_LINEAR;
}
void GXInitTexObjCI(GXTexObj*a,void*b,u16 c,u16 d,GXCITexFmt e,GXTexWrapMode f,GXTexWrapMode g,u8 h,u32 i){
    GXInitTexObj(a, b, c, d, (GXTexFmt)e, f, g, h);
    ((u32*)a)[TEXOBJ_CI_FORMAT] = e;
    ((u32*)a)[TEXOBJ_TLUT_NAME] = i;
}
void GXInitTexObjLOD(GXTexObj*a,GXTexFilter b,GXTexFilter c,f32 d,f32 e,f32 f,GXBool g,GXBool h,GXAnisotropy i){
    (void)d; (void)e; (void)f; (void)g; (void)h; (void)i;
    ((u32*)a)[TEXOBJ_MIN_FILTER] = b;
    ((u32*)a)[TEXOBJ_MAG_FILTER] = c;
}
void GXLoadTexObj(GXTexObj* a, GXTexMapID b) {
    gx_draw();
    if (b == GX_TEXMAP_NULL || b == GX_TEX_DISABLE) {
        gx_select_texmap(GX_TEXMAP_NULL);
    } else if (b >= GX_TEXMAP0 && b <= GX_TEXMAP7) {
        if (!s_tex_bindings[b].valid ||
            memcmp(s_tex_bindings[b].object_words, a,
                   sizeof(s_tex_bindings[b].object_words)) != 0) {
            memcpy(s_tex_bindings[b].object_words, a,
                   sizeof(s_tex_bindings[b].object_words));
            s_tex_bindings[b].refreshed_frame = 0;
        }
        s_tex_bindings[b].valid = 1;
        gx_refresh_binding(b);
        if (s_active_texmap == b) {
            gx_select_texmap(b);
        }
    }
}
void GXInitTlutObj(GXTlutObj*a,void*b,GXTlutFmt c,u16 d){
    u32* o = (u32*)a;
    o[TLUTOBJ_DATA] = (u32)(uintptr_t)b;
    o[TLUTOBJ_FORMAT] = c;
    o[TLUTOBJ_N_ENTRIES] = d;
}
void GXLoadTlut(GXTlutObj* a, u32 b) {
    u32* o = (u32*)a;
    u32 idx = b & 0xf;
    gx_draw();
    s_tluts[idx].data = (const void*)(uintptr_t)o[TLUTOBJ_DATA];
    s_tluts[idx].format = o[TLUTOBJ_FORMAT];
    s_tluts[idx].n_entries = (u16)o[TLUTOBJ_N_ENTRIES];
    s_tluts[idx].is_be = 1;
}

void GXSetTevOp(GXTevStageID stage,GXTevMode mode){
    gx_draw();
    if (stage < GX_MAX_TEVSTAGE) gx_tev_stage_set_mode(&s_tev_stages[stage], mode);
}
void GXSetTevColorIn(GXTevStageID stage,GXTevColorArg a,GXTevColorArg b,GXTevColorArg c,GXTevColorArg d){
    gx_draw();
    if (stage < GX_MAX_TEVSTAGE) {
        s_tev_stages[stage].color[0]=a;s_tev_stages[stage].color[1]=b;
        s_tev_stages[stage].color[2]=c;s_tev_stages[stage].color[3]=d;
    }
}
void GXSetTevAlphaIn(GXTevStageID stage,GXTevAlphaArg a,GXTevAlphaArg b,GXTevAlphaArg c,GXTevAlphaArg d){
    gx_draw();
    if (stage < GX_MAX_TEVSTAGE) {
        s_tev_stages[stage].alpha[0]=a;s_tev_stages[stage].alpha[1]=b;
        s_tev_stages[stage].alpha[2]=c;s_tev_stages[stage].alpha[3]=d;
    }
}
void GXSetTevColorOp(GXTevStageID stage,GXTevOp op,GXTevBias bias,GXTevScale scale,GXBool clamp,GXTevRegID out){
    gx_draw();
    if (stage < GX_MAX_TEVSTAGE) {
        s_tev_stages[stage].color_op=op;s_tev_stages[stage].color_bias=bias;
        s_tev_stages[stage].color_scale=scale;s_tev_stages[stage].color_clamp=clamp;
        s_tev_stages[stage].color_out=out;
    }
}
void GXSetTevAlphaOp(GXTevStageID stage,GXTevOp op,GXTevBias bias,GXTevScale scale,GXBool clamp,GXTevRegID out){
    gx_draw();
    if (stage < GX_MAX_TEVSTAGE) {
        s_tev_stages[stage].alpha_op=op;s_tev_stages[stage].alpha_bias=bias;
        s_tev_stages[stage].alpha_scale=scale;s_tev_stages[stage].alpha_clamp=clamp;
        s_tev_stages[stage].alpha_out=out;
    }
}
void GXSetTevColor(GXTevRegID reg, GXColor color) {
    gx_draw();
    if (reg <= GX_TEVREG2) s_tev_colors[reg] = color;
}
IGNORE2(GXSetTevKColorSel,GXTevStageID,GXTevKColorSel)
IGNORE2(GXSetTevKAlphaSel,GXTevStageID,GXTevKAlphaSel)
IGNORE3(GXSetTevSwapMode,GXTevStageID,GXTevSwapSel,GXTevSwapSel)
void GXSetTevSwapModeTable(GXTevSwapSel a,GXTevColorChan b,GXTevColorChan c,GXTevColorChan d,GXTevColorChan e){(void)a;(void)b;(void)c;(void)d;(void)e;}
void GXSetTevOrder(GXTevStageID a,GXTexCoordID b,GXTexMapID c,GXChannelID d){
    gx_draw();
    if (a < GX_MAX_TEVSTAGE) {
        s_tev_stages[a].texcoord = b;
        s_tev_stages[a].texmap = c;
        s_tev_stages[a].channel = d;
        gx_resolve_tev_texture();
    }
}
IGNORE1(GXSetTevDirect,GXTevStageID)
IGNORE3(GXSetIndTexCoordScale,GXIndTexStageID,GXIndTexScale,GXIndTexScale)

void GXSetAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op,
                       GXCompare comp1, u8 ref1) {
    gx_draw();
    s_alpha_comp0 = comp0;
    s_alpha_ref0 = ref0;
    s_alpha_op = op;
    s_alpha_comp1 = comp1;
    s_alpha_ref1 = ref1;
}
void GXSetBlendMode(GXBlendMode mode,GXBlendFactor src,GXBlendFactor dst,GXLogicOp op){
    (void)op;
    gx_draw();
    s_blend_mode = mode;
    s_blend_src = src;
    s_blend_dst = dst;
}
void GXSetColorUpdate(GXBool enable) {
    gx_draw();
    s_color_update = enable;
}
void GXSetAlphaUpdate(GXBool enable) {
    gx_draw();
    s_alpha_update = enable;
}
void GXSetZMode(GXBool compare_enable, GXCompare func, GXBool update_enable) {
    gx_draw();
    s_depth_test = compare_enable;
    s_depth_func = func;
    s_depth_write = update_enable;
}
IGNORE1(GXSetZCompLoc,GXBool)
IGNORE2(GXSetPixelFmt,GXPixelFmt,GXZFmt16)
IGNORE1(GXSetDither,GXBool)
IGNORE2(GXSetDstAlpha,GXBool,u8)
IGNORE2(GXSetFieldMask,GXBool,GXBool)
IGNORE2(GXSetFieldMode,GXBool,GXBool)
void GXSetFog(GXFogType a,f32 b,f32 c,f32 d,f32 e,GXColor f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;}
IGNORE3(GXSetFogRangeAdj,GXBool,u16,GXFogAdjTable*)
IGNORE3(GXSetZTexture,GXZTexOp,GXTexFmt,u32)

IGNORE2(GXCopyDisp,void*,GXBool)
IGNORE2(GXCopyTex,void*,GXBool)
IGNORE2(GXSetCopyClear,GXColor,u32)
IGNORE1(GXSetDispCopyGamma,GXGamma)
void GXSetDispCopySrc(u16 a,u16 b,u16 c,u16 d){(void)a;(void)b;(void)c;(void)d;}
IGNORE2(GXSetDispCopyDst,u16,u16)
u32 GXSetDispCopyYScale(f32 a){(void)a;return 0;}
void GXSetCopyFilter(GXBool a,const u8 b[12][2],GXBool c,const u8 d[7]){(void)a;(void)b;(void)c;(void)d;}
void GXSetTexCopySrc(u16 a,u16 b,u16 c,u16 d){(void)a;(void)b;(void)c;(void)d;}
void GXSetTexCopyDst(u16 a,u16 b,GXTexFmt c,GXBool d){(void)a;(void)b;(void)c;(void)d;}
IGNORE1(GXSetCopyClamp,GXFBClamp)
