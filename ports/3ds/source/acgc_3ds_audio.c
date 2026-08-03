#include "acgc_3ds_platform.h"

#include <3ds.h>
#include <string.h>

#define ACGC_AUDIO_SAMPLE_RATE 32000
#define ACGC_AUDIO_BUFFER_COUNT 4
#define ACGC_AUDIO_BUFFER_SAMPLES 2048

static int g_audio_ready;
static int g_audio_started;
static s16* g_audio_data;
static ndspWaveBuf g_audio_waves[ACGC_AUDIO_BUFFER_COUNT];

int acgc_3ds_audio_init(void) {
    if (g_audio_ready) return 1;
    if (ndspInit() != 0) {
        g_audio_ready = 0;
        return 0;
    }

    g_audio_data = linearAlloc(ACGC_AUDIO_BUFFER_COUNT * ACGC_AUDIO_BUFFER_SAMPLES *
                               2 * sizeof(s16));
    if (g_audio_data == NULL) {
        ndspExit();
        return 0;
    }
    memset(g_audio_data, 0, ACGC_AUDIO_BUFFER_COUNT * ACGC_AUDIO_BUFFER_SAMPLES *
                            2 * sizeof(s16));
    memset(g_audio_waves, 0, sizeof(g_audio_waves));

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(0, ACGC_AUDIO_SAMPLE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);

    g_audio_started = 0;
    g_audio_ready = 1;
    return 1;
}

int acgc_3ds_audio_submit(const void* samples, size_t byte_count, int volume) {
    size_t frames;
    int slot = -1;
    s16* dst;
    const s16* src = (const s16*)samples;

    if (!g_audio_ready || samples == NULL) return 0;
    frames = byte_count / (sizeof(s16) * 2);
    if (frames > ACGC_AUDIO_BUFFER_SAMPLES) frames = ACGC_AUDIO_BUFFER_SAMPLES;
    for (int i = 0; i < ACGC_AUDIO_BUFFER_COUNT; ++i) {
        if (g_audio_waves[i].status == NDSP_WBUF_FREE ||
            g_audio_waves[i].status == NDSP_WBUF_DONE) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return 0;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    dst = g_audio_data + slot * ACGC_AUDIO_BUFFER_SAMPLES * 2;
    for (size_t i = 0; i < frames * 2; ++i) {
        dst[i] = (s16)(((int)src[i] * volume) / 100);
    }
    DSP_FlushDataCache(dst, frames * 2 * sizeof(s16));
    memset(&g_audio_waves[slot], 0, sizeof(g_audio_waves[slot]));
    g_audio_waves[slot].data_vaddr = dst;
    g_audio_waves[slot].nsamples = frames;
    ndspChnWaveBufAdd(0, &g_audio_waves[slot]);
    g_audio_started = 1;
    return 1;
}

int acgc_3ds_audio_buffered_samples(void) {
    int samples = 0;
    for (int i = 0; i < ACGC_AUDIO_BUFFER_COUNT; ++i) {
        if (g_audio_waves[i].status == NDSP_WBUF_QUEUED ||
            g_audio_waves[i].status == NDSP_WBUF_PLAYING) {
            samples += (int)g_audio_waves[i].nsamples * 2;
        }
    }
    return samples;
}

int acgc_3ds_audio_ready(void) {
    return g_audio_ready;
}

void acgc_3ds_audio_shutdown(void) {
    if (g_audio_ready) {
        ndspChnReset(0);
        linearFree(g_audio_data);
        g_audio_data = NULL;
        ndspExit();
        g_audio_started = 0;
        g_audio_ready = 0;
    }
}
