/* pc_audio.c - SDL2 audio backend with dedicated producer thread.
 *
 * Architecture (matches GC):
 *   Game thread:  Na_GameFrame() queues commands via message queues
 *   Audio thread: pc_audio_process_frame() produces samples into ring buffer
 *   SDL callback: reads ring buffer → speakers
 *
 * The audio thread decouples sample production from the game frame,
 * so OS preemption of the game thread doesn't cause audio dropouts.
 */
#include "pc_platform.h"
#include "pc_settings.h"
#include "jaudio_NES/audiothread.h"

#define PC_AUDIO_SAMPLE_RATE 32000

#ifdef TARGET_3DS

#include <3ds/svc.h>
#include <3ds/thread.h>
#include "acgc_3ds_platform.h"

typedef void (*AIDMACallback)(void);
typedef void (*AIStreamCallback)(u32);

static AIDMACallback ai_dma_callback;
static AIStreamCallback ai_stream_callback;
static Thread audio_producer_thread;
static volatile bool audio_thread_running;
static bool audio_dma_running;
static u32 ai_dsp_sample_rate = PC_AUDIO_SAMPLE_RATE;
static u32 ai_stream_sample_rate = 48000;
static u32 ai_stream_trigger;
static u32 ai_stream_state;
static u32 ai_stream_samples;
static u8 ai_stream_left;
static u8 ai_stream_right;
static u32 ai_dma_addr;
static u32 ai_dma_length;

static void pc_audio_producer_func(void* data) {
    (void)data;
    while (audio_thread_running) {
        if (audio_dma_running && acgc_3ds_audio_buffered_samples() < 4480) {
            pc_audio_process_frame();
            /* Each game audio update produces 560 stereo frames at 32 kHz
             * (17.5 ms). Keep emulators that retire ndsp buffers instantly
             * from turning this worker into a busy loop. */
            svcSleepThread(17000000LL);
        } else {
            svcSleepThread(1000000LL);
        }
    }
}

void pc_audio_start_producer_thread(void) {
    if (audio_producer_thread) return;
    audio_thread_running = true;
    audio_producer_thread = threadCreate(pc_audio_producer_func, NULL, 32 * 1024,
                                         0x30, -2, false);
    if (!audio_producer_thread) audio_thread_running = false;
}

void AIInit(u8* stack) {
    (void)stack;
    acgc_3ds_audio_init();
}

void AIInitDMA(u32 addr, u32 size) {
    ai_dma_addr = addr;
    ai_dma_length = size;
    acgc_3ds_audio_submit((const void*)(uintptr_t)addr, size,
                          g_pc_settings.master_volume);
    ai_stream_samples += size / (sizeof(s16) * 2);
}

void AIStartDMA(void) { audio_dma_running = true; }
void AIStopDMA(void) { audio_dma_running = false; }
BOOL AIGetDMAEnableFlag(void) { return audio_dma_running ? TRUE : FALSE; }
u32 AIGetDMABytesLeft(void) { return (u32)acgc_3ds_audio_buffered_samples() * sizeof(s16); }
u32 AIGetDMAStartAddr(void) { return ai_dma_addr; }
u16 AIGetDMALength(void) { return (u16)ai_dma_length; }

void* AIRegisterDMACallback(void* callback) {
    void* old = (void*)ai_dma_callback;
    ai_dma_callback = (AIDMACallback)callback;
    return old;
}

void* AIRegisterStreamCallback(void* callback) {
    void* old = (void*)ai_stream_callback;
    ai_stream_callback = (AIStreamCallback)callback;
    return old;
}

u32 AIGetStreamTrigger(void) { return ai_stream_trigger; }
void AISetStreamTrigger(u32 trigger) { ai_stream_trigger = trigger; }
u32 AIGetStreamSampleCount(void) { return ai_stream_samples; }
void AIResetStreamSampleCount(void) { ai_stream_samples = 0; }
void AISetStreamPlayState(u32 state) { ai_stream_state = state; }
u32 AIGetStreamPlayState(void) { return ai_stream_state; }
void AISetStreamSampleRate(u32 rate) { ai_stream_sample_rate = rate; }
u32 AIGetStreamSampleRate(void) { return ai_stream_sample_rate; }
void AISetStreamVolLeft(u8 vol) { ai_stream_left = vol; }
void AISetStreamVolRight(u8 vol) { ai_stream_right = vol; }
u8 AIGetStreamVolLeft(void) { return ai_stream_left; }
u8 AIGetStreamVolRight(void) { return ai_stream_right; }
void AISetDSPSampleRate(u32 rate) { ai_dsp_sample_rate = rate; }
u32 AIGetDSPSampleRate(void) { return ai_dsp_sample_rate; }
void AIReset(void) { ai_stream_samples = 0; }

void DSPInit(void) {}
BOOL DSPCheckMailToDSP(void) { return FALSE; }
BOOL DSPCheckMailFromDSP(void) { return FALSE; }
u32 DSPReadMailFromDSP(void) { return 0; }
void DSPSendMailToDSP(u32 mail) { (void)mail; }
void DSPAssertInt(void) {}
void* DSPAddTask(void* task) { return task; }

int pc_audio_get_buffer_fill(void) { return acgc_3ds_audio_buffered_samples(); }
int pc_audio_is_active(void) { return acgc_3ds_audio_ready(); }
void pc_audio_set_paused(int paused) { audio_dma_running = !paused; }

void pc_audio_shutdown(void) {
    audio_thread_running = false;
    if (audio_producer_thread) {
        threadJoin(audio_producer_thread, UINT64_MAX);
        threadFree(audio_producer_thread);
        audio_producer_thread = NULL;
    }
    acgc_3ds_audio_shutdown();
}

#else

/* lock-free SPSC ring buffer (producer=audio thread, consumer=SDL callback) */
#define RING_BUF_SAMPLES (32768) /* ~512ms at 32kHz stereo */
#define RING_BUF_MASK    (RING_BUF_SAMPLES - 1)

/* Produce more samples when buffer drops below this level.
 * ~4 audio frames ahead = ~70ms of buffer at 32kHz stereo. */
#define AUDIO_PRODUCE_THRESHOLD 4480

static s16 ring_buffer[RING_BUF_SAMPLES];
static SDL_atomic_t ring_write_pos; /* written by audio producer thread */
static SDL_atomic_t ring_read_pos;  /* written by SDL audio callback */
static SDL_AudioDeviceID audio_device = 0;

typedef void (*AIDMACallback)(void);
static AIDMACallback ai_dma_callback = NULL;
static u32 ai_dsp_sample_rate = PC_AUDIO_SAMPLE_RATE;

/* --- Audio producer thread --- */
static SDL_Thread* audio_producer_thread = NULL;
static SDL_atomic_t audio_thread_running;

static int pc_audio_producer_func(void* data) {
    (void)data;
    while (SDL_AtomicGet(&audio_thread_running)) {
        int fill = pc_audio_get_buffer_fill();
        if (fill < AUDIO_PRODUCE_THRESHOLD) {
            pc_audio_process_frame();
        } else {
            SDL_Delay(1);
        }
    }
    return 0;
}

void pc_audio_start_producer_thread(void) {
    if (audio_producer_thread) return;
    SDL_AtomicSet(&audio_thread_running, 1);
    audio_producer_thread = SDL_CreateThread(pc_audio_producer_func, "AudioProducer", NULL);
    if (audio_producer_thread) {
        printf("[AUDIO] Producer thread started\n");
    } else {
        printf("[AUDIO] Failed to create producer thread: %s\n", SDL_GetError());
    }
}

/* --- SDL audio callback (runs on SDL's audio device thread) --- */
static void pc_audio_callback(void* userdata, Uint8* stream, int len) {
    s16* out = (s16*)stream;
    int total_samples = len / sizeof(s16);
    u32 wp = (u32)SDL_AtomicGet(&ring_write_pos);
    SDL_MemoryBarrierAcquire();
    u32 rp = (u32)SDL_AtomicGet(&ring_read_pos);
    u32 used = wp - rp;

    /* overrun: producer lapped us */
    if (used > RING_BUF_SAMPLES) {
        rp = wp - RING_BUF_SAMPLES;
        rp &= ~1u; /* stereo-align */
        used = wp - rp;
    }

    int avail = (int)used;
    avail &= ~1; /* whole stereo frames only */
    int copy = (avail < total_samples) ? avail : total_samples;
    copy &= ~1;

    for (int i = 0; i < copy; i++) {
        out[i] = ring_buffer[(rp + i) & RING_BUF_MASK];
    }
    if (copy < total_samples) {
        memset(&out[copy], 0, (total_samples - copy) * sizeof(s16));
    }

    SDL_MemoryBarrierRelease();
    SDL_AtomicSet(&ring_read_pos, (int)(rp + copy));
}

/* --- AI (Audio Interface) --- */

void AIInit(u8* stack) {
    (void)stack;
    if (audio_device != 0) return;

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = PC_AUDIO_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = pc_audio_callback;

    audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio_device != 0) {
        printf("[AUDIO] Opened: freq=%d fmt=0x%04X ch=%d samples=%d (requested: freq=%d)\n",
               have.freq, have.format, have.channels, have.samples, want.freq);
    } else {
        printf("[AUDIO] Failed to open: %s\n", SDL_GetError());
    }
}

void AIInitDMA(u32 addr, u32 size) {
    s16* src = (s16*)(uintptr_t)addr;
    u32 n_samples = size / sizeof(s16);
    n_samples &= ~1u; /* whole stereo frames */

    u32 wp = (u32)SDL_AtomicGet(&ring_write_pos);
    u32 rp = (u32)SDL_AtomicGet(&ring_read_pos);
    SDL_MemoryBarrierAcquire();
    u32 used = wp - rp;
    u32 free = RING_BUF_SAMPLES - used;

    if (n_samples > free) {
        n_samples = free & ~1u;
    }

    int vol = g_pc_settings.master_volume;
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;

    for (u32 i = 0; i < n_samples; i++) {
        int s = ((int)src[i] * vol) / 100;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        ring_buffer[(wp + i) & RING_BUF_MASK] = (s16)s;
    }

    SDL_MemoryBarrierRelease();
    SDL_AtomicSet(&ring_write_pos, (int)(wp + n_samples));
}

void AIStartDMA(void) {
    if (audio_device != 0) SDL_PauseAudioDevice(audio_device, 0);
}

void AIStopDMA(void) {
    if (audio_device != 0) SDL_PauseAudioDevice(audio_device, 1);
}

void pc_audio_set_paused(int paused) {
    /* Currently unused */
    if (audio_device != 0) SDL_PauseAudioDevice(audio_device, paused ? 1 : 0);
}

u32  AIGetDMAStartAddr(void) { return 0; }
u16  AIGetDMALength(void) { return 0; }
u32  AIGetStreamTrigger(void) { return 0; }
u32  AIGetStreamSampleCount(void) { return 0; }
void AISetStreamPlayState(u32 state) { (void)state; }
u32  AIGetStreamPlayState(void) { return 0; }
void AISetStreamSampleRate(u32 rate) { (void)rate; }
u32  AIGetStreamSampleRate(void) { return PC_AUDIO_SAMPLE_RATE; }
void AISetStreamVolLeft(u8 vol) { (void)vol; }
void AISetStreamVolRight(u8 vol) { (void)vol; }
u8   AIGetStreamVolLeft(void) { return 0; }
u8   AIGetStreamVolRight(void) { return 0; }
void AIResetStreamSampleCount(void) {}
void AISetDSPSampleRate(u32 rate) { ai_dsp_sample_rate = rate; }
u32  AIGetDSPSampleRate(void) { return ai_dsp_sample_rate; }

void* AIRegisterDMACallback(void* callback) {
    void* old = (void*)ai_dma_callback;
    ai_dma_callback = (AIDMACallback)callback;
    return old;
}

/* --- DSP stubs (rspsim does everything in software) --- */

void DSPInit(void) {}
BOOL DSPCheckMailToDSP(void) { return FALSE; }
BOOL DSPCheckMailFromDSP(void) { return FALSE; }
u32  DSPReadMailFromDSP(void) { return 0; }
void DSPSendMailToDSP(u32 mail) { (void)mail; }
void DSPAssertInt(void) {}
void* DSPAddTask(void* task) { return task; }

/* --- ring buffer queries for frame pacing (pc_vi.c) --- */

int pc_audio_get_buffer_fill(void) {
    return SDL_AtomicGet(&ring_write_pos) - SDL_AtomicGet(&ring_read_pos);
}

int pc_audio_is_active(void) {
    return audio_device != 0;
}

void pc_audio_shutdown(void) {
    /* Stop producer thread first */
    SDL_AtomicSet(&audio_thread_running, 0);
    if (audio_producer_thread) {
        SDL_WaitThread(audio_producer_thread, NULL);
        audio_producer_thread = NULL;
    }
    if (audio_device != 0) {
        SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
    }
}

#endif
