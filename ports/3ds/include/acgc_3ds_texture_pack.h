#ifndef ACGC_3DS_TEXTURE_PACK_H
#define ACGC_3DS_TEXTURE_PACK_H

#include <stddef.h>
#include <3ds/types.h>

int acgc_3ds_texture_pack_init(void);
void acgc_3ds_texture_pack_shutdown(void);

/* Returns a newly allocated RGBA8 image when a Dolphin texture-pack entry
 * matches. The caller owns the returned buffer. */
u8* acgc_3ds_texture_pack_lookup(const void* data, size_t data_size,
                                 int width, int height, u32 format,
                                 const void* tlut_data, int tlut_entries,
                                 int tlut_is_be, int* out_width,
                                 int* out_height);

#endif
