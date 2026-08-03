#include "acgc_3ds_paths.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int ensure_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 1 : 0;
    }

    return mkdir(path, 0777) == 0;
}

static int has_disc_extension(const char* name) {
    const char* dot = strrchr(name, '.');
    if (dot == NULL) {
        return 0;
    }

    return strcasecmp(dot, ".iso") == 0 ||
           strcasecmp(dot, ".gcm") == 0 ||
           strcasecmp(dot, ".ciso") == 0;
}

static unsigned long long file_size(const char* path) {
    FILE* fp = fopen(path, "rb");
    unsigned long long size = 0;

    if (fp == NULL) {
        return 0;
    }

    if (fseek(fp, 0, SEEK_END) == 0) {
        long pos = ftell(fp);
        if (pos > 0) {
            size = (unsigned long long)pos;
        }
    }

    fclose(fp);
    return size;
}

void acgc_3ds_probe_paths(Acgc3dsPathProbe* probe) {
    DIR* dir;
    struct dirent* ent;

    memset(probe, 0, sizeof(*probe));

    probe->base_dir_ready = ensure_dir(ACGC_3DS_BASE_DIR);
    probe->rom_dir_ready = ensure_dir(ACGC_3DS_ROM_DIR);
    probe->save_dir_ready = ensure_dir(ACGC_3DS_SAVE_DIR);
    probe->dump_dir_ready = ensure_dir(ACGC_3DS_DUMP_DIR);
    probe->texture_dir_ready = ensure_dir(ACGC_3DS_TEXTURE_DIR);

    if (!probe->rom_dir_ready) {
        return;
    }

    dir = opendir(ACGC_3DS_ROM_DIR);
    if (dir == NULL) {
        return;
    }

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.' || !has_disc_extension(ent->d_name)) {
            continue;
        }

        snprintf(probe->rom_path, sizeof(probe->rom_path), "%s/%s",
                 ACGC_3DS_ROM_DIR, ent->d_name);
        probe->rom_size = file_size(probe->rom_path);
        probe->rom_found = 1;
        break;
    }

    closedir(dir);
}

const char* acgc_3ds_format_size(unsigned long long size, char* out, size_t out_size) {
    if (size >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(out, out_size, "%llu MiB", size / (1024ULL * 1024ULL));
    } else if (size >= 1024ULL * 1024ULL) {
        snprintf(out, out_size, "%llu MiB", size / (1024ULL * 1024ULL));
    } else if (size >= 1024ULL) {
        snprintf(out, out_size, "%llu KiB", size / 1024ULL);
    } else {
        snprintf(out, out_size, "%llu bytes", size);
    }

    return out;
}
