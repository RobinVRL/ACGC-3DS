#ifndef ACGC_3DS_DISC_H
#define ACGC_3DS_DISC_H

#include <stdint.h>

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;

typedef struct Acgc3dsDiscInfo {
    int is_ciso;
    char game_id[7];
    u32 dol_offset;
    u32 dol_size;
    int fst_file_count;
} Acgc3dsDiscInfo;

int acgc_3ds_disc_init_path(const char* path);
int acgc_3ds_disc_is_open(void);
int acgc_3ds_disc_get_info(Acgc3dsDiscInfo* info);
int acgc_3ds_disc_find_file(const char* path, u32* disc_offset, u32* file_size);
int acgc_3ds_disc_read(u32 offset, void* dest, u32 size);
u8* acgc_3ds_disc_extract_dol(u32* out_size);
u8* acgc_3ds_disc_read_file(const char* path, u32* out_size);
u8* acgc_3ds_disc_extract_rel(u32* out_size, int* out_was_yaz0);
int acgc_3ds_disc_dump_rel(const char* output_path, u32* out_size, int* out_was_yaz0);
void acgc_3ds_disc_shutdown(void);

#endif
