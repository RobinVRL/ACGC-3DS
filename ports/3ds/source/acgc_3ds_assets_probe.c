#include "acgc_3ds_assets_probe.h"

#include <string.h>

enum {
    SRC_REL = 0,
    SRC_DOL = 1,
    SWAP_NONE = 0,
    SWAP_VTX = 2
};

static u32 checksum32(const u8* data, u32 size) {
    u32 hash = 2166136261U;
    u32 i;

    for (i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= 16777619U;
    }

    return hash;
}

static void bswap_vtx(void* data, u32 size) {
    u8* p = (u8*)data;
    u32 count = size / 16;
    u32 i;

    for (i = 0; i < count; i++) {
        int j;
        for (j = 0; j < 12; j += 2) {
            u8 t = p[j];
            p[j] = p[j + 1];
            p[j + 1] = t;
        }
        p += 16;
    }
}

static int load_sample(const u8* dol, u32 dol_size, const u8* rel, u32 rel_size,
                       int source, u32 offset, void* dest, u32 size, int swap) {
    const u8* src = source == SRC_REL ? rel : dol;
    u32 src_size = source == SRC_REL ? rel_size : dol_size;

    if (src == NULL || offset > src_size || size > src_size - offset) {
        return 0;
    }

    memcpy(dest, src + offset, size);
    if (swap == SWAP_VTX) {
        bswap_vtx(dest, size);
    }

    return 1;
}

void acgc_3ds_assets_probe(const u8* dol, u32 dol_size,
                           const u8* rel, u32 rel_size,
                           Acgc3dsAssetProbeResult* out) {
    u8 rel_sample[0x20];
    u8 dol_sample[0x0C];
    u8 vtx_sample[0x40];

    memset(out, 0, sizeof(*out));

    out->rel_sample_ok = load_sample(dol, dol_size, rel, rel_size,
                                     SRC_REL, 0x355740, rel_sample,
                                     sizeof(rel_sample), SWAP_NONE);
    if (out->rel_sample_ok) {
        out->rel_checksum = checksum32(rel_sample, sizeof(rel_sample));
    }

    out->dol_sample_ok = load_sample(dol, dol_size, rel, rel_size,
                                     SRC_DOL, 0xAD5E8, dol_sample,
                                     sizeof(dol_sample), SWAP_NONE);
    if (out->dol_sample_ok) {
        memcpy(out->creator, dol_sample, sizeof(dol_sample));
        out->creator[sizeof(out->creator) - 1] = '\0';
        out->dol_checksum = checksum32(dol_sample, sizeof(dol_sample));
    }

    out->dol_vtx_sample_ok = load_sample(dol, dol_size, rel, rel_size,
                                         SRC_DOL, 0xC00C0, vtx_sample,
                                         sizeof(vtx_sample), SWAP_VTX);
    if (out->dol_vtx_sample_ok) {
        out->vtx_checksum = checksum32(vtx_sample, sizeof(vtx_sample));
    }

    out->ok = out->rel_sample_ok && out->dol_sample_ok && out->dol_vtx_sample_ok;
}

