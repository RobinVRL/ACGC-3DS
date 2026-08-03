#ifndef ACGC_3DS_PATHS_H
#define ACGC_3DS_PATHS_H

#include <stddef.h>

#define ACGC_3DS_BASE_DIR "sdmc:/3ds/acgc"
#define ACGC_3DS_ROM_DIR  ACGC_3DS_BASE_DIR "/rom"
#define ACGC_3DS_SAVE_DIR ACGC_3DS_BASE_DIR "/save"
#define ACGC_3DS_DUMP_DIR ACGC_3DS_BASE_DIR "/dump"
#define ACGC_3DS_TEXTURE_DIR ACGC_3DS_BASE_DIR "/textures"

typedef struct Acgc3dsPathProbe {
    int base_dir_ready;
    int rom_dir_ready;
    int save_dir_ready;
    int dump_dir_ready;
    int texture_dir_ready;
    int rom_found;
    char rom_path[320];
    unsigned long long rom_size;
} Acgc3dsPathProbe;

void acgc_3ds_probe_paths(Acgc3dsPathProbe* probe);
const char* acgc_3ds_format_size(unsigned long long size, char* out, size_t out_size);

#endif
