/* Dolphin-compatible DDS texture-pack support for the 3DS renderer. */
#include "acgc_3ds_texture_pack.h"
#include "acgc_3ds_paths.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef unsigned long long u64x;

typedef struct TexPackEntry {
    u64x data_hash;
    u64x tlut_hash;
    u32 width;
    u32 height;
    u32 format;
    char* path;
    int wildcard;
} TexPackEntry;

static TexPackEntry* s_entries;
static size_t s_entry_count;
static size_t s_entry_capacity;
static int s_initialized;

#define XXH_P1 0x9E3779B185EBCA87ULL
#define XXH_P2 0xC2B2AE3D27D4EB4FULL
#define XXH_P3 0x165667B19E3779F9ULL
#define XXH_P4 0x85EBCA77C2B2AE63ULL
#define XXH_P5 0x27D4EB2F165667C5ULL

static u64x read64(const void* ptr) { u64x v; memcpy(&v, ptr, 8); return v; }
static u32 read32(const void* ptr) { u32 v; memcpy(&v, ptr, 4); return v; }
static u64x rotl64(u64x v, int bits) { return (v << bits) | (v >> (64 - bits)); }
static u64x xxh_round(u64x acc, u64x input) {
    acc += input * XXH_P2;
    return rotl64(acc, 31) * XXH_P1;
}
static u64x xxh_merge(u64x acc, u64x v) {
    acc ^= xxh_round(0, v);
    return acc * XXH_P1 + XXH_P4;
}

static u64x xxhash64(const void* input, size_t length) {
    const u8* p = (const u8*)input;
    const u8* end = p + length;
    u64x hash;
    if (length >= 32) {
        const u8* limit = end - 32;
        u64x v1 = XXH_P1 + XXH_P2, v2 = XXH_P2, v3 = 0, v4 = 0 - XXH_P1;
        do {
            v1 = xxh_round(v1, read64(p)); p += 8;
            v2 = xxh_round(v2, read64(p)); p += 8;
            v3 = xxh_round(v3, read64(p)); p += 8;
            v4 = xxh_round(v4, read64(p)); p += 8;
        } while (p <= limit);
        hash = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        hash = xxh_merge(hash, v1); hash = xxh_merge(hash, v2);
        hash = xxh_merge(hash, v3); hash = xxh_merge(hash, v4);
    } else {
        hash = XXH_P5;
    }
    hash += length;
    while (p + 8 <= end) {
        hash ^= xxh_round(0, read64(p));
        hash = rotl64(hash, 27) * XXH_P1 + XXH_P4;
        p += 8;
    }
    if (p + 4 <= end) {
        hash ^= (u64x)read32(p) * XXH_P1;
        hash = rotl64(hash, 23) * XXH_P2 + XXH_P3;
        p += 4;
    }
    while (p < end) {
        hash ^= (u64x)*p++ * XXH_P5;
        hash = rotl64(hash, 11) * XXH_P1;
    }
    hash ^= hash >> 33; hash *= XXH_P2;
    hash ^= hash >> 29; hash *= XXH_P3;
    return hash ^ (hash >> 32);
}

static int hex64(const char* text, u64x* out) {
    u64x v = 0;
    for (int i = 0; i < 16; i++) {
        int digit;
        char c = text[i];
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return 0;
        v = (v << 4) | (u64x)digit;
    }
    *out = v;
    return 1;
}

static int parse_name(const char* name, TexPackEntry* entry) {
    char* end;
    const char* p;
    unsigned long value;
    if (strncmp(name, "tex1_", 5) != 0) return 0;
    p = name + 5;
    value = strtoul(p, &end, 10); if (end == p || *end != 'x') return 0;
    entry->width = (u32)value;
    p = end + 1; value = strtoul(p, &end, 10); if (end == p || *end != '_') return 0;
    entry->height = (u32)value;
    p = end + 1; if (!hex64(p, &entry->data_hash) || p[16] != '_') return 0;
    p += 17;
    entry->tlut_hash = 0;
    entry->wildcard = 0;
    if (*p == '$') {
        entry->wildcard = 1;
        if (p[1] != '_') return 0;
        p += 2;
    } else {
        const char* underscore = strchr(p, '_');
        const char* dot = strchr(p, '.');
        if (underscore != NULL && (dot == NULL || underscore < dot)) {
            if (!hex64(p, &entry->tlut_hash) || p[16] != '_') return 0;
            p += 17;
        }
    }
    value = strtoul(p, &end, 10);
    if (end == p || strcasecmp(end, ".dds") != 0) return 0;
    entry->format = (u32)value;
    return entry->width != 0 && entry->height != 0;
}

static int append_entry(const TexPackEntry* parsed, const char* path) {
    TexPackEntry* grown;
    size_t path_len;
    if (s_entry_count == s_entry_capacity) {
        size_t capacity = s_entry_capacity == 0 ? 256 : s_entry_capacity * 2;
        grown = (TexPackEntry*)realloc(s_entries, capacity * sizeof(*grown));
        if (grown == NULL) return 0;
        s_entries = grown;
        s_entry_capacity = capacity;
    }
    s_entries[s_entry_count] = *parsed;
    path_len = strlen(path) + 1;
    s_entries[s_entry_count].path = (char*)malloc(path_len);
    if (s_entries[s_entry_count].path == NULL) return 0;
    memcpy(s_entries[s_entry_count].path, path, path_len);
    s_entry_count++;
    return 1;
}

static void scan_dir(const char* path, int depth) {
    DIR* dir;
    struct dirent* item;
    if (depth > 12 || (dir = opendir(path)) == NULL) return;
    while ((item = readdir(dir)) != NULL) {
        char full[512];
        struct stat st;
        TexPackEntry parsed;
        if (item->d_name[0] == '.') continue;
        if (snprintf(full, sizeof(full), "%s/%s", path, item->d_name) >= (int)sizeof(full)) continue;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) scan_dir(full, depth + 1);
        else if (parse_name(item->d_name, &parsed)) append_entry(&parsed, full);
    }
    closedir(dir);
}

static int entry_compare(const void* left, const void* right) {
    const TexPackEntry* a = (const TexPackEntry*)left;
    const TexPackEntry* b = (const TexPackEntry*)right;
    if (a->data_hash < b->data_hash) return -1;
    if (a->data_hash > b->data_hash) return 1;
    return 0;
}

int acgc_3ds_texture_pack_init(void) {
    struct stat st;
    if (s_initialized) return s_entry_count != 0;
    s_initialized = 1;
    if (stat(ACGC_3DS_TEXTURE_DIR, &st) != 0) mkdir(ACGC_3DS_TEXTURE_DIR, 0777);
    scan_dir(ACGC_3DS_TEXTURE_DIR, 0);
    if (s_entry_count > 1) qsort(s_entries, s_entry_count, sizeof(*s_entries), entry_compare);
    if (s_entry_count != 0)
        printf("[TexturePack] Indexed %lu DDS replacements from %s\n",
               (unsigned long)s_entry_count, ACGC_3DS_TEXTURE_DIR);
    return s_entry_count != 0;
}

void acgc_3ds_texture_pack_shutdown(void) {
    for (size_t i = 0; i < s_entry_count; i++) free(s_entries[i].path);
    free(s_entries);
    s_entries = NULL;
    s_entry_count = s_entry_capacity = 0;
    s_initialized = 0;
}

static TexPackEntry* find_entry(u64x data_hash, u64x tlut_hash,
                                u32 format, u32 width, u32 height) {
    TexPackEntry* wildcard = NULL;
    size_t low = 0, high = s_entry_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (s_entries[mid].data_hash < data_hash) low = mid + 1;
        else high = mid;
    }
    for (size_t i = low; i < s_entry_count && s_entries[i].data_hash == data_hash; i++) {
        TexPackEntry* e = &s_entries[i];
        if (e->data_hash != data_hash || e->format != format ||
            e->width != width || e->height != height) continue;
        if (!e->wildcard && e->tlut_hash == tlut_hash) return e;
        if (e->wildcard && tlut_hash != 0) wildcard = e;
    }
    return wildcard;
}

static u16 rgb565(const u8* p) { return (u16)p[0] | ((u16)p[1] << 8); }
static void color565(u16 c, u8 out[4]) {
    out[0] = (u8)(((c >> 11) & 31) * 255 / 31);
    out[1] = (u8)(((c >> 5) & 63) * 255 / 63);
    out[2] = (u8)((c & 31) * 255 / 31);
    out[3] = 255;
}

static void bc_colors(const u8* block, u8 colors[4][4], int force_four) {
    u16 c0 = rgb565(block), c1 = rgb565(block + 2);
    color565(c0, colors[0]); color565(c1, colors[1]);
    if (c0 > c1 || force_four) {
        for (int c = 0; c < 3; c++) {
            colors[2][c] = (u8)((2 * colors[0][c] + colors[1][c]) / 3);
            colors[3][c] = (u8)((colors[0][c] + 2 * colors[1][c]) / 3);
        }
        colors[2][3] = colors[3][3] = 255;
    } else {
        for (int c = 0; c < 3; c++) colors[2][c] = (u8)((colors[0][c] + colors[1][c]) / 2);
        colors[2][3] = 255;
        memset(colors[3], 0, 4);
    }
}

static void decode_bc_pixel(const u8* data, int width, int x, int y,
                            int bc3, u8 out[4]) {
    int blocks_x = (width + 3) / 4;
    const u8* block = data + ((y / 4) * blocks_x + x / 4) * (bc3 ? 16 : 8);
    const u8* colors_block = block + (bc3 ? 8 : 0);
    u8 colors[4][4];
    u32 selectors = read32(colors_block + 4);
    int pixel = (y & 3) * 4 + (x & 3);
    bc_colors(colors_block, colors, bc3);
    memcpy(out, colors[(selectors >> (pixel * 2)) & 3], 4);
    if (bc3) {
        u8 alpha[8];
        u64x alpha_bits = 0;
        alpha[0] = block[0]; alpha[1] = block[1];
        if (alpha[0] > alpha[1]) {
            for (int i = 1; i <= 6; i++) alpha[i + 1] = (u8)(((7 - i) * alpha[0] + i * alpha[1]) / 7);
        } else {
            for (int i = 1; i <= 4; i++) alpha[i + 1] = (u8)(((5 - i) * alpha[0] + i * alpha[1]) / 5);
            alpha[6] = 0; alpha[7] = 255;
        }
        for (int i = 0; i < 6; i++) alpha_bits |= (u64x)block[2 + i] << (i * 8);
        out[3] = alpha[(alpha_bits >> (pixel * 3)) & 7];
    }
}

enum { DDS_RGBA, DDS_BGRA, DDS_BC1, DDS_BC3 };

static u8* load_dds(const char* path, int* out_width, int* out_height) {
    FILE* fp = fopen(path, "rb");
    u8 header[148];
    u8* encoded = NULL;
    u8* rgba = NULL;
    u32 width, height, flags, fourcc, dxgi = 0, bits, rmask, bmask;
    int type = -1, block_size = 0;
    int target_w, target_h;
    size_t encoded_size;
    if (fp == NULL || fread(header, 1, 128, fp) != 128 || read32(header) != 0x20534444) goto fail;
    height = read32(header + 12); width = read32(header + 16);
    flags = read32(header + 80); fourcc = read32(header + 84);
    bits = read32(header + 88); rmask = read32(header + 92); bmask = read32(header + 100);
    if (width == 0 || height == 0 || width > 16384 || height > 16384) goto fail;
    if ((flags & 4) && fourcc == 0x30315844) {
        if (fread(header + 128, 1, 20, fp) != 20) goto fail;
        dxgi = read32(header + 128);
        if (dxgi == 71) type = DDS_BC1;
        else if (dxgi == 77) type = DDS_BC3;
        else if (dxgi == 28) type = DDS_RGBA;
        else if (dxgi == 87) type = DDS_BGRA;
    } else if (flags & 4) {
        if (fourcc == 0x31545844) type = DDS_BC1;
        else if (fourcc == 0x35545844) type = DDS_BC3;
    } else if (bits == 32) {
        type = (rmask == 0x00ff0000 && bmask == 0x000000ff) ? DDS_BGRA : DDS_RGBA;
    }
    if (type < 0) goto fail;
    block_size = type == DDS_BC1 ? 8 : 16;
    encoded_size = type == DDS_BC1 || type == DDS_BC3 ?
                   (size_t)((width + 3) / 4) * ((height + 3) / 4) * block_size :
                   (size_t)width * height * 4;
    if (encoded_size > 32u * 1024u * 1024u) goto fail;
    encoded = (u8*)malloc(encoded_size);
    if (encoded == NULL || fread(encoded, 1, encoded_size, fp) != encoded_size) goto fail;
    fclose(fp); fp = NULL;

    target_w = (int)width; target_h = (int)height;
    while (target_w > 1024 || target_h > 1024) {
        target_w = (target_w + 1) / 2;
        target_h = (target_h + 1) / 2;
    }
    rgba = (u8*)malloc((size_t)target_w * target_h * 4);
    if (rgba == NULL) goto fail;
    for (int y = 0; y < target_h; y++) {
        int sy = (int)((u64x)y * height / (u32)target_h);
        for (int x = 0; x < target_w; x++) {
            int sx = (int)((u64x)x * width / (u32)target_w);
            u8* dst = rgba + ((size_t)y * target_w + x) * 4;
            if (type == DDS_BC1 || type == DDS_BC3)
                decode_bc_pixel(encoded, (int)width, sx, sy, type == DDS_BC3, dst);
            else {
                const u8* src = encoded + ((size_t)sy * width + sx) * 4;
                dst[0] = type == DDS_BGRA ? src[2] : src[0];
                dst[1] = src[1];
                dst[2] = type == DDS_BGRA ? src[0] : src[2];
                dst[3] = src[3];
            }
        }
    }
    free(encoded);
    *out_width = target_w; *out_height = target_h;
    return rgba;
fail:
    if (fp != NULL) fclose(fp);
    free(encoded); free(rgba);
    return NULL;
}

u8* acgc_3ds_texture_pack_lookup(const void* data, size_t data_size,
                                 int width, int height, u32 format,
                                 const void* tlut_data, int tlut_entries,
                                 int tlut_is_be, int* out_width,
                                 int* out_height) {
    u64x data_hash, tlut_hash = 0;
    TexPackEntry* entry;
    if (!s_initialized) acgc_3ds_texture_pack_init();
    if (s_entry_count == 0 || data == NULL || data_size == 0) return NULL;
    data_hash = xxhash64(data, data_size);
    if (tlut_data != NULL && tlut_entries > 0) {
        const u8* bytes = (const u8*)data;
        unsigned min = 0xffff, max = 0;
        if (tlut_entries <= 16) {
            for (size_t i = 0; i < data_size; i++) {
                unsigned a = bytes[i] >> 4, b = bytes[i] & 15;
                if (a < min) min = a;
                if (b < min) min = b;
                if (a > max) max = a;
                if (b > max) max = b;
            }
        } else {
            for (size_t i = 0; i < data_size; i++) {
                unsigned v = bytes[i];
                if (v < min) min = v;
                if (v > max) max = v;
            }
        }
        if (min > max) min = max = 0;
        if (max >= (unsigned)tlut_entries) max = (unsigned)tlut_entries - 1;
        {
            size_t count = (size_t)(max - min + 1) * 2;
            const u8* source = (const u8*)tlut_data + min * 2;
            if (tlut_is_be) tlut_hash = xxhash64(source, count);
            else {
                u8* swapped = (u8*)malloc(count);
                if (swapped == NULL) return NULL;
                for (size_t i = 0; i < count; i += 2) {
                    swapped[i] = source[i + 1]; swapped[i + 1] = source[i];
                }
                tlut_hash = xxhash64(swapped, count);
                free(swapped);
            }
        }
    }
    entry = find_entry(data_hash, tlut_hash, format, (u32)width, (u32)height);
    if (entry == NULL) return NULL;
    return load_dds(entry->path, out_width, out_height);
}
