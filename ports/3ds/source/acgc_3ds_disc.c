#include "acgc_3ds_disc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CISO_HDR_SIZE 0x8000
#define CISO_MAGIC    0x4F534943
#define CISO_MAP_OFF  8
#define GC_MAGIC      0xC2339F3D
#define MAX_FST_FILES 1024
#define REL_IO_BUFFER_SIZE (8 * 1024)
#define YAZ0_WINDOW_SIZE   0x1000

typedef struct DiscFile {
    FILE* fp;
    int is_ciso;
    u32 block_size;
    int num_blocks;
    int* block_phys;
} DiscFile;

typedef struct FSTFile {
    char path[256];
    u32 disc_offset;
    u32 file_size;
} FSTFile;

static DiscFile g_disc;
static int g_disc_open;
static u32 g_dol_offset;
static u32 g_dol_size;
static char g_game_id[7];
static FSTFile g_fst_files[MAX_FST_FILES];
static int g_fst_file_count;

static u32 be32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u32 le32(const u8* p) {
    return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u8* yaz0_decode(const u8* src, u32 src_size, u32* out_size) {
    u32 dec_size;
    u32 sp;
    u32 dp;
    u8* dst;
    int bit;

    if (src_size < 16 || memcmp(src, "Yaz0", 4) != 0) {
        return NULL;
    }

    dec_size = be32(src + 4);
    dst = (u8*)malloc(dec_size);
    if (dst == NULL) {
        return NULL;
    }

    sp = 16;
    dp = 0;
    while (dp < dec_size && sp < src_size) {
        u8 flags = src[sp++];

        for (bit = 7; bit >= 0 && dp < dec_size; bit--) {
            if (sp >= src_size) {
                break;
            }

            if ((flags & (1 << bit)) != 0) {
                dst[dp++] = src[sp++];
            } else {
                u8 b1;
                u8 b2;
                u32 dist;
                u32 len;
                u32 ref;

                if (sp + 1 >= src_size) {
                    break;
                }

                b1 = src[sp++];
                b2 = src[sp++];
                dist = ((u32)(b1 & 0x0F) << 8) | b2;
                if ((b1 >> 4) == 0) {
                    if (sp >= src_size) {
                        break;
                    }
                    len = (u32)src[sp++] + 0x12;
                } else {
                    len = (u32)(b1 >> 4) + 2;
                }

                ref = dp - dist - 1;
                while (len-- > 0 && dp < dec_size) {
                    dst[dp++] = dst[ref++];
                }
            }
        }
    }

    if (dp != dec_size) {
        free(dst);
        return NULL;
    }

    if (out_size != NULL) {
        *out_size = dec_size;
    }

    return dst;
}

static void disc_close(DiscFile* df) {
    if (df->fp != NULL) {
        fclose(df->fp);
    }
    free(df->block_phys);
    memset(df, 0, sizeof(*df));
}

static int disc_open(DiscFile* df, const char* path) {
    u8* hdr;

    memset(df, 0, sizeof(*df));
    df->fp = fopen(path, "rb");
    if (df->fp == NULL) {
        return 0;
    }

    hdr = (u8*)malloc(CISO_HDR_SIZE);
    if (hdr == NULL) {
        disc_close(df);
        return 0;
    }

    if (fread(hdr, 1, CISO_HDR_SIZE, df->fp) == CISO_HDR_SIZE &&
        le32(hdr) == CISO_MAGIC) {
        df->block_size = le32(hdr + 4);
        if (df->block_size > 0) {
            int i;
            int phys = 0;

            df->num_blocks = CISO_HDR_SIZE - CISO_MAP_OFF;
            df->block_phys = (int*)malloc((size_t)df->num_blocks * sizeof(int));
            if (df->block_phys == NULL) {
                free(hdr);
                disc_close(df);
                return 0;
            }

            for (i = 0; i < df->num_blocks; i++) {
                df->block_phys[i] = hdr[CISO_MAP_OFF + i] ? phys++ : -1;
            }

            df->is_ciso = 1;
            free(hdr);
            return 1;
        }
    }

    df->is_ciso = 0;
    free(hdr);
    return 1;
}

static int disc_read(DiscFile* df, u32 offset, void* dest, u32 size) {
    if (!df->is_ciso) {
        if (fseek(df->fp, (long)offset, SEEK_SET) != 0) {
            return 0;
        }
        return (u32)fread(dest, 1, size, df->fp) == size;
    }

    {
        u8* out = (u8*)dest;
        while (size > 0) {
            u32 bi = offset / df->block_size;
            u32 bo = offset % df->block_size;
            u32 chunk = df->block_size - bo;
            if (chunk > size) {
                chunk = size;
            }

            if ((int)bi >= df->num_blocks || df->block_phys[bi] < 0) {
                memset(out, 0, chunk);
            } else {
                u32 phys = CISO_HDR_SIZE + (u32)df->block_phys[bi] * df->block_size + bo;
                if (fseek(df->fp, (long)phys, SEEK_SET) != 0) {
                    return 0;
                }
                if ((u32)fread(out, 1, chunk, df->fp) != chunk) {
                    return 0;
                }
            }

            out += chunk;
            offset += chunk;
            size -= chunk;
        }
    }

    return 1;
}

static int gcm_verify(DiscFile* df) {
    u8 buf[4];
    return disc_read(df, 0x1C, buf, sizeof(buf)) && be32(buf) == GC_MAGIC;
}

static u32 gcm_dol_offset_read(DiscFile* df) {
    u8 buf[4];
    if (!disc_read(df, 0x420, buf, sizeof(buf))) {
        return 0;
    }
    return be32(buf);
}

static u32 gcm_dol_size_calc(DiscFile* df, u32 dol_off) {
    u8 hdr[0xE4];
    u32 max_end = 0;
    int i;

    if (!disc_read(df, dol_off, hdr, sizeof(hdr))) {
        return 0;
    }

    for (i = 0; i < 7; i++) {
        u32 off = be32(hdr + i * 4);
        u32 sz = be32(hdr + 0x90 + i * 4);
        if (off + sz > max_end) {
            max_end = off + sz;
        }
    }
    for (i = 0; i < 11; i++) {
        u32 off = be32(hdr + 0x1C + i * 4);
        u32 sz = be32(hdr + 0xAC + i * 4);
        if (off + sz > max_end) {
            max_end = off + sz;
        }
    }

    return max_end;
}

static void build_fst_table(DiscFile* df) {
    u8 buf[12];
    u32 fst_off;
    u32 num_ent;
    u32 str_tbl;
    u32 i;
    struct {
        u32 next_entry;
        char name[128];
    } dir_stack[32];
    int stack_depth = 0;

    g_fst_file_count = 0;

    if (!disc_read(df, 0x424, buf, 4)) {
        return;
    }
    fst_off = be32(buf);

    if (!disc_read(df, fst_off + 8, buf, 4)) {
        return;
    }
    num_ent = be32(buf);
    str_tbl = fst_off + num_ent * 12;

    dir_stack[0].next_entry = num_ent;
    dir_stack[0].name[0] = '\0';
    stack_depth = 1;

    for (i = 1; i < num_ent; i++) {
        u32 noff;
        char name[128];

        while (stack_depth > 0 && i >= dir_stack[stack_depth - 1].next_entry) {
            stack_depth--;
        }

        if (!disc_read(df, fst_off + i * 12, buf, 12)) {
            return;
        }
        noff = ((u32)buf[1] << 16) | ((u32)buf[2] << 8) | buf[3];
        if (!disc_read(df, str_tbl + noff, name, sizeof(name) - 1)) {
            return;
        }
        name[sizeof(name) - 1] = '\0';

        if (buf[0] == 1) {
            if (stack_depth < 32) {
                dir_stack[stack_depth].next_entry = be32(buf + 8);
                snprintf(dir_stack[stack_depth].name,
                         sizeof(dir_stack[stack_depth].name), "%s", name);
                stack_depth++;
            }
        } else if (g_fst_file_count < MAX_FST_FILES) {
            char path[256];
            int d;

            path[0] = '\0';
            for (d = 1; d < stack_depth; d++) {
                strncat(path, dir_stack[d].name, sizeof(path) - strlen(path) - 2);
                strncat(path, "/", sizeof(path) - strlen(path) - 1);
            }
            strncat(path, name, sizeof(path) - strlen(path) - 1);

            snprintf(g_fst_files[g_fst_file_count].path,
                     sizeof(g_fst_files[g_fst_file_count].path), "%s", path);
            g_fst_files[g_fst_file_count].disc_offset = be32(buf + 4);
            g_fst_files[g_fst_file_count].file_size = be32(buf + 8);
            g_fst_file_count++;
        }
    }
}

int acgc_3ds_disc_init_path(const char* path) {
    FILE* diag;
    if (g_disc_open) {
        disc_close(&g_disc);
        g_disc_open = 0;
    }

    if (!disc_open(&g_disc, path)) {
        diag = fopen("sdmc:/3ds/acgc/disc_diag.txt", "w");
        if (diag != NULL) {
            fprintf(diag, "open failed path=%s\n", path != NULL ? path : "(null)");
            fclose(diag);
        }
        return 0;
    }

    if (!gcm_verify(&g_disc)) {
        diag = fopen("sdmc:/3ds/acgc/disc_diag.txt", "w");
        if (diag != NULL) {
            fprintf(diag, "verify failed path=%s\n", path != NULL ? path : "(null)");
            fclose(diag);
        }
        disc_close(&g_disc);
        return 0;
    }

    if (!disc_read(&g_disc, 0, g_game_id, 6)) {
        diag = fopen("sdmc:/3ds/acgc/disc_diag.txt", "w");
        if (diag != NULL) {
            fprintf(diag, "header read failed path=%s\n", path != NULL ? path : "(null)");
            fclose(diag);
        }
        disc_close(&g_disc);
        return 0;
    }
    g_game_id[6] = '\0';

    g_dol_offset = gcm_dol_offset_read(&g_disc);
    g_dol_size = gcm_dol_size_calc(&g_disc, g_dol_offset);
    build_fst_table(&g_disc);

    g_disc_open = 1;
    diag = fopen("sdmc:/3ds/acgc/disc_diag.txt", "w");
    if (diag != NULL) {
        fprintf(diag, "open ok path=%s game_id=%s dol=0x%08lx size=%lu fst=%d ciso=%d\n",
                path != NULL ? path : "(null)", g_game_id,
                (unsigned long)g_dol_offset, (unsigned long)g_dol_size,
                g_fst_file_count, g_disc.is_ciso);
        fclose(diag);
    }
    return 1;
}

int acgc_3ds_disc_is_open(void) {
    return g_disc_open;
}

int acgc_3ds_disc_get_info(Acgc3dsDiscInfo* info) {
    if (!g_disc_open || info == NULL) {
        return 0;
    }

    memset(info, 0, sizeof(*info));
    info->is_ciso = g_disc.is_ciso;
    memcpy(info->game_id, g_game_id, sizeof(info->game_id));
    info->dol_offset = g_dol_offset;
    info->dol_size = g_dol_size;
    info->fst_file_count = g_fst_file_count;
    return 1;
}

int acgc_3ds_disc_find_file(const char* path, u32* disc_offset, u32* file_size) {
    int i;

    if (!g_disc_open || path == NULL) {
        return 0;
    }

    if (path[0] == '/') {
        path++;
    }

    for (i = 0; i < g_fst_file_count; i++) {
        if (strcmp(g_fst_files[i].path, path) == 0) {
            if (disc_offset != NULL) {
                *disc_offset = g_fst_files[i].disc_offset;
            }
            if (file_size != NULL) {
                *file_size = g_fst_files[i].file_size;
            }
            return 1;
        }
    }

    return 0;
}

int acgc_3ds_disc_read(u32 offset, void* dest, u32 size) {
    if (!g_disc_open) {
        return 0;
    }

    return disc_read(&g_disc, offset, dest, size);
}

u8* acgc_3ds_disc_extract_dol(u32* out_size) {
    u8* buf;

    if (!g_disc_open || g_dol_size == 0) {
        return NULL;
    }

    buf = (u8*)malloc(g_dol_size);
    if (buf == NULL) {
        return NULL;
    }

    if (!disc_read(&g_disc, g_dol_offset, buf, g_dol_size)) {
        free(buf);
        return NULL;
    }

    if (out_size != NULL) {
        *out_size = g_dol_size;
    }

    return buf;
}

u8* acgc_3ds_disc_read_file(const char* path, u32* out_size) {
    u32 off;
    u32 size;
    u8* buf;

    if (!acgc_3ds_disc_find_file(path, &off, &size)) {
        return NULL;
    }

    buf = (u8*)malloc(size);
    if (buf == NULL) {
        return NULL;
    }

    if (!disc_read(&g_disc, off, buf, size)) {
        free(buf);
        return NULL;
    }

    if (out_size != NULL) {
        *out_size = size;
    }

    return buf;
}

u8* acgc_3ds_disc_extract_rel(u32* out_size, int* out_was_yaz0) {
    u32 raw_size;
    u8* raw = acgc_3ds_disc_read_file("foresta.rel.szs", &raw_size);

    if (out_was_yaz0 != NULL) {
        *out_was_yaz0 = 0;
    }

    if (raw == NULL) {
        return NULL;
    }

    if (raw_size >= 16 && memcmp(raw, "Yaz0", 4) == 0) {
        u32 dec_size = 0;
        u8* dec = yaz0_decode(raw, raw_size, &dec_size);

        free(raw);
        if (dec == NULL) {
            return NULL;
        }

        if (out_size != NULL) {
            *out_size = dec_size;
        }
        if (out_was_yaz0 != NULL) {
            *out_was_yaz0 = 1;
        }
        return dec;
    }

    if (out_size != NULL) {
        *out_size = raw_size;
    }

    return raw;
}

typedef struct RelStreamReader {
    u32 disc_offset;
    u32 file_size;
    u32 position;
    u32 buffer_start;
    u32 buffer_size;
    u8 buffer[REL_IO_BUFFER_SIZE];
} RelStreamReader;

static int rel_stream_read_byte(RelStreamReader* reader, u8* value) {
    u32 buffer_end = reader->buffer_start + reader->buffer_size;

    if (reader->position >= reader->file_size) {
        return 0;
    }

    if (reader->position < reader->buffer_start || reader->position >= buffer_end) {
        u32 remaining = reader->file_size - reader->position;
        reader->buffer_start = reader->position;
        reader->buffer_size = remaining < REL_IO_BUFFER_SIZE ? remaining : REL_IO_BUFFER_SIZE;
        if (!disc_read(&g_disc, reader->disc_offset + reader->buffer_start,
                       reader->buffer, reader->buffer_size)) {
            reader->buffer_size = 0;
            return 0;
        }
    }

    *value = reader->buffer[reader->position - reader->buffer_start];
    reader->position++;
    return 1;
}

static int rel_stream_flush(FILE* fp, u8* output, u32* output_used) {
    if (*output_used != 0 && fwrite(output, 1, *output_used, fp) != *output_used) {
        return 0;
    }
    *output_used = 0;
    return 1;
}

static int rel_stream_write_byte(FILE* fp, u8 value, u8* output,
                                 u32* output_used, u8* window, u32 decoded) {
    output[(*output_used)++] = value;
    window[decoded & (YAZ0_WINDOW_SIZE - 1)] = value;
    if (*output_used == REL_IO_BUFFER_SIZE) {
        return rel_stream_flush(fp, output, output_used);
    }
    return 1;
}

int acgc_3ds_disc_dump_rel(const char* output_path, u32* out_size, int* out_was_yaz0) {
    RelStreamReader reader;
    FILE* fp = NULL;
    u8* output = NULL;
    u8 header[16];
    u8 window[YAZ0_WINDOW_SIZE];
    u32 file_offset;
    u32 file_size;
    u32 output_used = 0;
    u32 decoded = 0;
    u32 decoded_size;
    int was_yaz0;
    int ok = 0;
    int i;

    if (out_size != NULL) {
        *out_size = 0;
    }
    if (out_was_yaz0 != NULL) {
        *out_was_yaz0 = 0;
    }
    if (output_path == NULL ||
        !acgc_3ds_disc_find_file("foresta.rel.szs", &file_offset, &file_size) ||
        file_size < sizeof(header)) {
        return 0;
    }

    memset(&reader, 0, sizeof(reader));
    reader.disc_offset = file_offset;
    reader.file_size = file_size;
    for (i = 0; i < (int)sizeof(header); i++) {
        if (!rel_stream_read_byte(&reader, &header[i])) {
            return 0;
        }
    }

    was_yaz0 = memcmp(header, "Yaz0", 4) == 0;
    decoded_size = was_yaz0 ? be32(header + 4) : file_size;
    if (decoded_size == 0) {
        return 0;
    }

    output = (u8*)malloc(REL_IO_BUFFER_SIZE);
    if (output == NULL) {
        return 0;
    }
    fp = fopen(output_path, "wb");
    if (fp == NULL) {
        free(output);
        return 0;
    }

    if (!was_yaz0) {
        if (fwrite(header, 1, sizeof(header), fp) != sizeof(header)) {
            goto done;
        }
        decoded = sizeof(header);
        while (decoded < file_size) {
            u8 value;
            if (!rel_stream_read_byte(&reader, &value) ||
                !rel_stream_write_byte(fp, value, output, &output_used, window, decoded)) {
                goto done;
            }
            decoded++;
        }
    } else {
        while (decoded < decoded_size) {
            u8 flags;
            int bit;

            if (!rel_stream_read_byte(&reader, &flags)) {
                goto done;
            }
            for (bit = 7; bit >= 0 && decoded < decoded_size; bit--) {
                if ((flags & (1 << bit)) != 0) {
                    u8 value;
                    if (!rel_stream_read_byte(&reader, &value) ||
                        !rel_stream_write_byte(fp, value, output, &output_used,
                                               window, decoded)) {
                        goto done;
                    }
                    decoded++;
                } else {
                    u8 b1;
                    u8 b2;
                    u32 distance;
                    u32 length;
                    u32 reference;

                    if (!rel_stream_read_byte(&reader, &b1) ||
                        !rel_stream_read_byte(&reader, &b2)) {
                        goto done;
                    }
                    distance = ((u32)(b1 & 0x0F) << 8) | b2;
                    if (distance >= decoded) {
                        goto done;
                    }
                    if ((b1 >> 4) == 0) {
                        u8 extra;
                        if (!rel_stream_read_byte(&reader, &extra)) {
                            goto done;
                        }
                        length = (u32)extra + 0x12;
                    } else {
                        length = (u32)(b1 >> 4) + 2;
                    }
                    reference = decoded - distance - 1;
                    while (length-- != 0 && decoded < decoded_size) {
                        u8 value = window[reference & (YAZ0_WINDOW_SIZE - 1)];
                        if (!rel_stream_write_byte(fp, value, output, &output_used,
                                                   window, decoded)) {
                            goto done;
                        }
                        reference++;
                        decoded++;
                    }
                }
            }
        }
    }

    if (!rel_stream_flush(fp, output, &output_used)) {
        goto done;
    }
    ok = 1;

done:
    if (fclose(fp) != 0) {
        ok = 0;
    }
    free(output);
    if (!ok) {
        remove(output_path);
        return 0;
    }
    if (out_size != NULL) {
        *out_size = decoded_size;
    }
    if (out_was_yaz0 != NULL) {
        *out_was_yaz0 = was_yaz0;
    }
    return 1;
}

void acgc_3ds_disc_shutdown(void) {
    if (g_disc_open) {
        disc_close(&g_disc);
    }

    g_disc_open = 0;
    g_dol_offset = 0;
    g_dol_size = 0;
    g_game_id[0] = '\0';
    g_fst_file_count = 0;
}
