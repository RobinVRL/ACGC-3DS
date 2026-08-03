#include "acgc_3ds_loader.h"

#include <string.h>

static u32 be32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void add_dol_section(Acgc3dsDolMap* map, u32 file_offset, u32 load_addr,
                            u32 size, int is_text) {
    Acgc3dsDolSection* section;

    if (size == 0 || map->section_count >= ACGC_3DS_DOL_MAX_SECTIONS) {
        return;
    }

    section = &map->sections[map->section_count++];
    section->file_offset = file_offset;
    section->load_addr = load_addr;
    section->size = size;
    section->is_text = is_text;
}

int acgc_3ds_parse_dol(const u8* dol, u32 dol_size, Acgc3dsDolMap* out) {
    int i;

    if (dol == NULL || out == NULL || dol_size < 0x100) {
        return 0;
    }

    memset(out, 0, sizeof(*out));

    for (i = 0; i < 7; i++) {
        add_dol_section(out,
                        be32(dol + i * 4),
                        be32(dol + 0x48 + i * 4),
                        be32(dol + 0x90 + i * 4),
                        1);
    }

    for (i = 0; i < 11; i++) {
        add_dol_section(out,
                        be32(dol + 0x1C + i * 4),
                        be32(dol + 0x64 + i * 4),
                        be32(dol + 0xAC + i * 4),
                        0);
    }

    out->bss_addr = be32(dol + 0xD8);
    out->bss_size = be32(dol + 0xDC);
    out->entry_point = be32(dol + 0xE0);

    return out->section_count > 0;
}

int acgc_3ds_parse_rel(const u8* rel, u32 rel_size, Acgc3dsRelMap* out) {
    u32 section_info_offset;
    u32 section_count;
    u32 i;

    if (rel == NULL || out == NULL || rel_size < 0x4C) {
        return 0;
    }

    memset(out, 0, sizeof(*out));

    section_count = be32(rel + 0x0C);
    section_info_offset = be32(rel + 0x10);
    if (section_count > ACGC_3DS_REL_MAX_SECTIONS ||
        section_info_offset > rel_size ||
        section_info_offset + section_count * 8 > rel_size) {
        return 0;
    }

    out->id = be32(rel + 0x00);
    out->section_count = (int)section_count;
    out->version = be32(rel + 0x3C);
    out->bss_size = be32(rel + 0x20);
    out->prolog_section = be32(rel + 0x30);
    out->epilog_section = be32(rel + 0x34);
    out->unresolved_section = be32(rel + 0x38);

    for (i = 0; i < section_count; i++) {
        u32 raw_offset = be32(rel + section_info_offset + i * 8);

        out->sections[i].file_offset = raw_offset & ~1U;
        out->sections[i].executable = (raw_offset & 1U) != 0;
        out->sections[i].size = be32(rel + section_info_offset + i * 8 + 4);
    }

    return 1;
}

int acgc_3ds_dol_addr_to_offset(const Acgc3dsDolMap* map, u32 addr, u32* out_offset) {
    int i;

    if (map == NULL || out_offset == NULL) {
        return 0;
    }

    for (i = 0; i < map->section_count; i++) {
        const Acgc3dsDolSection* section = &map->sections[i];
        if (section->load_addr <= addr && addr < section->load_addr + section->size) {
            *out_offset = section->file_offset + (addr - section->load_addr);
            return 1;
        }
    }

    return 0;
}

int acgc_3ds_rel_section_offset_to_file(const Acgc3dsRelMap* map, int section,
                                        u32 section_offset, u32* out_offset) {
    const Acgc3dsRelSection* rel_section;

    if (map == NULL || out_offset == NULL || section < 0 || section >= map->section_count) {
        return 0;
    }

    rel_section = &map->sections[section];
    if (rel_section->file_offset == 0 || section_offset >= rel_section->size) {
        return 0;
    }

    *out_offset = rel_section->file_offset + section_offset;
    return 1;
}

