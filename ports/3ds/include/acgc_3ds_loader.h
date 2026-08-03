#ifndef ACGC_3DS_LOADER_H
#define ACGC_3DS_LOADER_H

#include "acgc_3ds_disc.h"

#define ACGC_3DS_DOL_MAX_SECTIONS 18
#define ACGC_3DS_REL_MAX_SECTIONS 32

typedef struct Acgc3dsDolSection {
    u32 file_offset;
    u32 load_addr;
    u32 size;
    int is_text;
} Acgc3dsDolSection;

typedef struct Acgc3dsDolMap {
    int section_count;
    u32 entry_point;
    u32 bss_addr;
    u32 bss_size;
    Acgc3dsDolSection sections[ACGC_3DS_DOL_MAX_SECTIONS];
} Acgc3dsDolMap;

typedef struct Acgc3dsRelSection {
    u32 file_offset;
    u32 size;
    int executable;
} Acgc3dsRelSection;

typedef struct Acgc3dsRelMap {
    u32 id;
    u32 version;
    u32 bss_size;
    u32 prolog_section;
    u32 epilog_section;
    u32 unresolved_section;
    int section_count;
    Acgc3dsRelSection sections[ACGC_3DS_REL_MAX_SECTIONS];
} Acgc3dsRelMap;

int acgc_3ds_parse_dol(const u8* dol, u32 dol_size, Acgc3dsDolMap* out);
int acgc_3ds_parse_rel(const u8* rel, u32 rel_size, Acgc3dsRelMap* out);
int acgc_3ds_dol_addr_to_offset(const Acgc3dsDolMap* map, u32 addr, u32* out_offset);
int acgc_3ds_rel_section_offset_to_file(const Acgc3dsRelMap* map, int section,
                                        u32 section_offset, u32* out_offset);

#endif

