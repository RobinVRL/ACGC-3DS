#ifndef ACGC_3DS_BOOT_SCENE_H
#define ACGC_3DS_BOOT_SCENE_H

#include <stddef.h>
#include <stdint.h>

int acgc_3ds_boot_scene_load(const uint8_t* dol, size_t dol_size);
void acgc_3ds_boot_scene_draw(void);
void acgc_3ds_boot_scene_reset(void);

#endif
