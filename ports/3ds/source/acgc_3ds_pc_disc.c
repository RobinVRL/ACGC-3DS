#include "pc_disc.h"

#include "acgc_3ds_disc.h"
#include "acgc_3ds_loader.h"
#include "acgc_3ds_paths.h"

#include <stdio.h>
#include <stdlib.h>

static u8* load_dump_file(const char* path, u32* out_size) {
    FILE* fp;
    long length;
    u8* data;

    if (out_size != NULL) *out_size = 0;
    fp = fopen(path, "rb");
    if (fp == NULL) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    length = ftell(fp);
    if (length <= 0 || (unsigned long)length > 0xFFFFFFFFUL ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    data = (u8*)malloc((size_t)length);
    if (data == NULL) {
        fclose(fp);
        return NULL;
    }
    if (fread(data, 1, (size_t)length, fp) != (size_t)length) {
        free(data);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    if (out_size != NULL) *out_size = (u32)length;
    return data;
}

int pc_disc_init(void) {
    Acgc3dsPathProbe probe;
    acgc_3ds_probe_paths(&probe);
    if (!probe.rom_found) return 0;
    return acgc_3ds_disc_init_path(probe.rom_path);
}

int pc_disc_is_open(void) {
    return acgc_3ds_disc_is_open();
}

int pc_disc_find_file(const char* path, u32* disc_offset, u32* file_size) {
    return acgc_3ds_disc_find_file(path, disc_offset, file_size);
}

int pc_disc_read(u32 offset, void* dest, u32 size) {
    return acgc_3ds_disc_read(offset, dest, size);
}

u8* pc_disc_extract_dol(void) {
    u32 size = 0;
    u8* data = load_dump_file(ACGC_3DS_DUMP_DIR "/main.dol", &size);
    Acgc3dsDolMap map;

    if (data != NULL) {
        if (acgc_3ds_parse_dol(data, size, &map)) {
            printf("Using dumped main.dol (%lu bytes)\n", (unsigned long)size);
            return data;
        }
        printf("Ignoring invalid dumped main.dol\n");
        free(data);
    }
    return acgc_3ds_disc_extract_dol(&size);
}

u8* pc_disc_extract_rel(void) {
    u32 size = 0;
    int was_yaz0;
    u8* data = load_dump_file(ACGC_3DS_DUMP_DIR "/foresta.rel", &size);
    Acgc3dsRelMap map;

    if (data != NULL) {
        if (acgc_3ds_parse_rel(data, size, &map)) {
            printf("Using dumped foresta.rel (%lu bytes)\n", (unsigned long)size);
            return data;
        }
        printf("Ignoring invalid dumped foresta.rel\n");
        free(data);
    }
    return acgc_3ds_disc_extract_rel(&size, &was_yaz0);
}

void pc_disc_shutdown(void) {
    acgc_3ds_disc_shutdown();
}
