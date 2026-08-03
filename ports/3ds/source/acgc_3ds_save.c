#include "acgc_3ds_platform.h"
#include "acgc_3ds_paths.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int g_save_ready;

static int ensure_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 1 : 0;
    }
    return mkdir(path, 0777) == 0;
}

int acgc_3ds_save_init(void) {
    g_save_ready = ensure_dir(ACGC_3DS_SAVE_DIR) &&
                   ensure_dir(ACGC_3DS_SAVE_DIR "/card_a") &&
                   ensure_dir(ACGC_3DS_SAVE_DIR "/card_b");
    return g_save_ready;
}

int acgc_3ds_save_ready(void) {
    return g_save_ready;
}

int acgc_3ds_save_path(char* out, size_t out_size, int chan, const char* file_name) {
    const char* slot = chan == 1 ? "card_b" : "card_a";

    if (out == NULL || file_name == NULL || strstr(file_name, "..") != NULL ||
        strchr(file_name, '/') != NULL || strchr(file_name, '\\') != NULL) {
        return 0;
    }

    snprintf(out, out_size, "%s/%s/%s", ACGC_3DS_SAVE_DIR, slot, file_name);
    return 1;
}

