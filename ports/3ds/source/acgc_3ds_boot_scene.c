#include "acgc_3ds_boot_scene.h"

#include "acgc_3ds_platform.h"

#include <string.h>

#define LOGO_NIN_VTX_OFFSET 0xC00C0u
#define LOGO_NIN_VTX_COUNT 4u

static Acgc3dsGpuVertex g_logo_vertices[6];
static int g_logo_ready;

static int16_t read_be_s16(const uint8_t* p) {
    return (int16_t)(((uint16_t)p[0] << 8) | p[1]);
}

int acgc_3ds_boot_scene_load(const uint8_t* dol, size_t dol_size) {
    Acgc3dsGpuVertex source[LOGO_NIN_VTX_COUNT];
    static const uint8_t indices[6] = {0, 1, 2, 0, 2, 3};
    float min_x = 32767.0f, max_x = -32768.0f;
    float min_y = 32767.0f, max_y = -32768.0f;
    float scale, width, height;
    unsigned i;

    g_logo_ready = 0;
    memset(source, 0, sizeof(source));
    if (dol == NULL || dol_size < LOGO_NIN_VTX_OFFSET + LOGO_NIN_VTX_COUNT * 16u) {
        return 0;
    }

    for (i = 0; i < LOGO_NIN_VTX_COUNT; ++i) {
        const uint8_t* v = dol + LOGO_NIN_VTX_OFFSET + i * 16u;
        float x = (float)read_be_s16(v);
        float y = (float)read_be_s16(v + 2);
        source[i].x = x;
        source[i].y = y;
        source[i].z = 0.5f;
        source[i].r = v[12] / 255.0f;
        source[i].g = v[13] / 255.0f;
        source[i].b = v[14] / 255.0f;
        source[i].a = v[15] / 255.0f;
        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
    }

    width = max_x - min_x;
    height = max_y - min_y;
    if (width < 1.0f || height < 1.0f) return 0;
    scale = 280.0f / width;
    if (height * scale > 150.0f) scale = 150.0f / height;

    for (i = 0; i < LOGO_NIN_VTX_COUNT; ++i) {
        source[i].x = 200.0f + (source[i].x - (min_x + max_x) * 0.5f) * scale;
        source[i].y = 120.0f - (source[i].y - (min_y + max_y) * 0.5f) * scale;
        if (source[i].a == 0.0f) {
            source[i].r = 0.85f;
            source[i].g = 0.12f;
            source[i].b = 0.10f;
            source[i].a = 1.0f;
        }
    }
    for (i = 0; i < 6; ++i) g_logo_vertices[i] = source[indices[i]];
    g_logo_ready = 1;
    return 1;
}

void acgc_3ds_boot_scene_draw(void) {
    if (g_logo_ready) acgc_3ds_video_draw_triangles(g_logo_vertices, 6);
}

void acgc_3ds_boot_scene_reset(void) {
    memset(g_logo_vertices, 0, sizeof(g_logo_vertices));
    g_logo_ready = 0;
}
