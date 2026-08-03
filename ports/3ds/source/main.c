#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>

#include "acgc_3ds_assets_probe.h"
#include "acgc_3ds_boot_scene.h"
#include "acgc_3ds_disc.h"
#include "acgc_3ds_loader.h"
#include "acgc_3ds_paths.h"
#include "acgc_3ds_platform.h"
#include "pc_assets.h"

extern void ac_entry(void);
extern int boot_main(int argc, const char** argv);
extern int g_pc_running;
int pc_disc_init(void);
void pc_disc_shutdown(void);

/* Keep MEM1 out of the fragmented ordinary heap. This reserves the full
 * 24 MiB GameCube arena plus 12 MiB for Citro3D textures, vertices, command
 * buffers, and ndsp audio. The remaining application memory becomes the
 * ordinary heap used by ARAM emulation and decoded ROM assets. */
u32 __ctru_linear_heap_size = 36 * 1024 * 1024;

static PrintConsole* g_launcher_console;

static bool swallow_console_char(void* console, int c) {
    (void)console;
    (void)c;
    return true;
}

static int write_file(const char* path, const void* data, u32 size) {
    FILE* fp = fopen(path, "wb");
    int ok;

    if (fp == NULL) {
        return 0;
    }

    ok = fwrite(data, 1, size, fp) == size;
    if (fclose(fp) != 0) {
        ok = 0;
    }
    return ok;
}

static void dump_loaded_binaries(void) {
    u8* dol;
    u32 dol_size = 0;
    u32 rel_size = 0;
    int rel_was_yaz0 = 0;
    int wrote_any = 0;

    if (!acgc_3ds_disc_is_open()) {
        printf("Dump: disc is not open\n");
        return;
    }

    dol = acgc_3ds_disc_extract_dol(&dol_size);
    if (dol != NULL) {
        if (write_file(ACGC_3DS_DUMP_DIR "/main.dol", dol, dol_size)) {
            printf("Dumped main.dol (%lu bytes)\n", (unsigned long)dol_size);
            wrote_any = 1;
        } else {
            printf("Dump main.dol failed\n");
        }
        free(dol);
    } else {
        printf("Dump main.dol failed\n");
    }

    if (acgc_3ds_disc_dump_rel(ACGC_3DS_DUMP_DIR "/foresta.rel",
                               &rel_size, &rel_was_yaz0)) {
        printf("Dumped foresta.rel (%lu bytes%s)\n",
               (unsigned long)rel_size,
               rel_was_yaz0 ? ", decoded" : "");
        wrote_any = 1;
    } else {
        printf("Dump foresta.rel failed\n");
    }

    if (wrote_any) {
        printf("Dump dir: %s\n", ACGC_3DS_DUMP_DIR);
    }
}

static void print_probe_status(const Acgc3dsPathProbe* probe) {
    Acgc3dsPlatformStatus platform;
    Acgc3dsDiscInfo disc_info;
    char size_buf[32];
    int disc_ready = 0;
    u32 copydate_off = 0;
    u32 copydate_size = 0;

    acgc_3ds_boot_scene_reset();

    printf("Animal Crossing GC 3DS port scaffold\n");
    printf("--------------------------------\n");
    acgc_3ds_platform_get_status(&platform);
    printf("video:%s audio:%s save:%s\n",
           platform.video_ready ? "OK" : "NO",
           platform.audio_ready ? "OK" : "NO",
           platform.save_ready ? "OK" : "NO");
    printf("pad: %04X lx:%d ly:%d\n",
           platform.pad.button,
           platform.pad.stick_x,
           platform.pad.stick_y);
    printf("base: %s\n", probe->base_dir_ready ? "ready" : "missing");
    printf("rom : %s\n", probe->rom_dir_ready ? "ready" : "missing");
    printf("save: %s\n", probe->save_dir_ready ? "ready" : "missing");
    printf("dump: %s\n", probe->dump_dir_ready ? "ready" : "missing");
    printf("textures: %s\n\n", probe->texture_dir_ready ? "ready" : "missing");

    if (probe->rom_found) {
        printf("ROM found:\n%s\n", probe->rom_path);
        printf("size: %s\n\n",
               acgc_3ds_format_size(probe->rom_size, size_buf, sizeof(size_buf)));

        acgc_3ds_disc_shutdown();
        disc_ready = acgc_3ds_disc_init_path(probe->rom_path);
        if (disc_ready && acgc_3ds_disc_get_info(&disc_info)) {
            u8* dol;
            u8* rel;
            u32 dol_size = 0;
            u32 rel_size = 0;
            int rel_was_yaz0 = 0;

            printf("Disc reader: OK\n");
            printf("format: %s\n", disc_info.is_ciso ? "CISO" : "ISO/GCM");
            printf("game id: %s\n", disc_info.game_id);
            printf("DOL: 0x%08lX, %lu bytes\n",
                   (unsigned long)disc_info.dol_offset,
                   (unsigned long)disc_info.dol_size);
            printf("FST files: %d\n", disc_info.fst_file_count);

            if (acgc_3ds_disc_find_file("COPYDATE", &copydate_off, &copydate_size)) {
                printf("COPYDATE: 0x%08lX, %lu bytes\n\n",
                       (unsigned long)copydate_off,
                       (unsigned long)copydate_size);
            } else {
                printf("COPYDATE: not found\n\n");
            }

            dol = acgc_3ds_disc_extract_dol(&dol_size);
            rel = acgc_3ds_disc_extract_rel(&rel_size, &rel_was_yaz0);

            if (dol != NULL) {
                Acgc3dsDolMap dol_map;
                printf("DOL load: OK (%lu bytes)\n", (unsigned long)dol_size);
                printf("Top renderer: %s\n",
                       acgc_3ds_boot_scene_load(dol, dol_size) ? "ROM mesh ready" : "mesh failed");
                if (acgc_3ds_parse_dol(dol, dol_size, &dol_map)) {
                    printf("DOL map: %d sections, entry 0x%08lX\n",
                           dol_map.section_count,
                           (unsigned long)dol_map.entry_point);
                    printf("DOL bss: 0x%08lX, %lu bytes\n",
                           (unsigned long)dol_map.bss_addr,
                           (unsigned long)dol_map.bss_size);
                } else {
                    printf("DOL map: parse failed\n");
                }
            } else {
                printf("DOL load: failed\n");
            }

            if (rel != NULL) {
                Acgc3dsRelMap rel_map;
                printf("REL load: OK (%lu bytes%s)\n",
                       (unsigned long)rel_size,
                       rel_was_yaz0 ? ", Yaz0" : "");
                if (acgc_3ds_parse_rel(rel, rel_size, &rel_map)) {
                    printf("REL map: id %lu, %d sections\n",
                           (unsigned long)rel_map.id,
                           rel_map.section_count);
                    printf("REL bss: %lu bytes, version %lu\n",
                           (unsigned long)rel_map.bss_size,
                           (unsigned long)rel_map.version);
                } else {
                    printf("REL map: parse failed\n");
                }
            } else {
                printf("REL load: failed\n");
            }

            if (dol != NULL && rel != NULL) {
                Acgc3dsAssetProbeResult asset_probe;
                acgc_3ds_assets_probe(dol, dol_size, rel, rel_size, &asset_probe);
                printf("Asset probe: %s\n", asset_probe.ok ? "OK" : "partial");
                printf(" REL sample: %s 0x%08lX\n",
                       asset_probe.rel_sample_ok ? "OK" : "fail",
                       (unsigned long)asset_probe.rel_checksum);
                printf(" DOL sample: %s %s\n",
                       asset_probe.dol_sample_ok ? "OK" : "fail",
                       asset_probe.dol_sample_ok ? asset_probe.creator : "");
                printf(" VTX swap: %s 0x%08lX\n",
                       asset_probe.dol_vtx_sample_ok ? "OK" : "fail",
                       (unsigned long)asset_probe.vtx_checksum);
            } else {
                printf("Asset probe: skipped\n");
            }

            if (dol != NULL) {
                free(dol);
            }
            if (rel != NULL) {
                free(rel);
            }
        } else {
            printf("Disc reader: failed\n");
            printf("Expected a valid GameCube image.\n\n");
        }
    } else {
        printf("No .iso/.gcm/.ciso found in:\n");
        printf("%s\n\n", ACGC_3DS_ROM_DIR);
    }

    printf("Press SELECT to rescan.\n");
    printf("Press Y to dump DOL/REL.\n");
    printf("Press X to launch native runtime.\n");
    printf("Press START to exit.\n");
}

static void launch_game(void) {
    consoleClear();
    printf("Opening ROM image...\n");
    pc_disc_shutdown();
    if (!pc_disc_init()) {
        printf("ROM open failed; press SELECT to rescan.\n");
        return;
    }
    printf("Loading game assets...\n");
    if (!pc_assets_init()) {
        printf("Asset load failed; press SELECT to rescan.\n");
        return;
    }
    printf("Starting Animal Crossing...\n");
    /* consoleInit owns the bottom framebuffer while the ROM launcher is
     * visible. Stop all console writes and return that screen to Citro3D only
     * after loading has succeeded. */
    if (g_launcher_console != NULL) {
        g_launcher_console->PrintChar = swallow_console_char;
    }
    consoleDebugInit(debugDevice_NULL);
    acgc_3ds_video_enable_game_screens();
    acgc_3ds_platform_begin_frame();
    ac_entry();
    boot_main(0, NULL);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    Acgc3dsPathProbe probe;

    /* Create the SD layout before platform initialization so the texture-pack
     * index sees a newly created /textures directory on the first launch. */
    acgc_3ds_probe_paths(&probe);
    if (!acgc_3ds_platform_init()) return 1;
    g_launcher_console = consoleInit(GFX_BOTTOM, NULL);

    if (!probe.rom_found) {
        print_probe_status(&probe);
    } else {
        launch_game();
        if (!g_pc_running) goto shutdown;
    }

    while (aptMainLoop()) {
        acgc_3ds_platform_begin_frame();
        u32 keys = acgc_3ds_input_keys_down();

        if (keys & KEY_START) {
            break;
        }

        if (keys & KEY_SELECT) {
            consoleClear();
            acgc_3ds_probe_paths(&probe);
            print_probe_status(&probe);
        }

        if (keys & KEY_Y) {
            dump_loaded_binaries();
        }

        if (keys & KEY_X) {
            /* launch_game enters the game's own frame loop. Never leave the
             * launcher's Citro3D frame open underneath it. */
            acgc_3ds_platform_end_frame();
            launch_game();
            if (!g_pc_running) break;
            continue;
        }

        acgc_3ds_platform_end_frame();
    }

shutdown:
    acgc_3ds_disc_shutdown();
    acgc_3ds_boot_scene_reset();
    acgc_3ds_platform_shutdown();
    return 0;
}
