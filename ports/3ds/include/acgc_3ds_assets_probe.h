#ifndef ACGC_3DS_ASSETS_PROBE_H
#define ACGC_3DS_ASSETS_PROBE_H

#include "acgc_3ds_disc.h"

typedef struct Acgc3dsAssetProbeResult {
    int ok;
    int rel_sample_ok;
    int dol_sample_ok;
    int dol_vtx_sample_ok;
    char creator[13];
    u32 rel_checksum;
    u32 dol_checksum;
    u32 vtx_checksum;
} Acgc3dsAssetProbeResult;

void acgc_3ds_assets_probe(const u8* dol, u32 dol_size,
                           const u8* rel, u32 rel_size,
                           Acgc3dsAssetProbeResult* out);

#endif

