#include "jaudio_NES/os.h"
#include "dolphin/os.h"
#include "jaudio_NES/dummyrom.h"
#include "jaudio_NES/sample.h"

#ifdef TARGET_PC
#ifdef TARGET_3DS
#include <3ds/svc.h>
#include <3ds/synchronization.h>
static LightLock z_mq_mutex;
static int z_mq_mutex_ready;

void pc_audio_mq_init(void) {
    if (!z_mq_mutex_ready) {
        LightLock_Init(&z_mq_mutex);
        z_mq_mutex_ready = 1;
    }
}

void pc_audio_mq_shutdown(void) {
    z_mq_mutex_ready = 0;
}

static void pc_mq_lock(void) {
    if (z_mq_mutex_ready) LightLock_Lock(&z_mq_mutex);
}

static void pc_mq_unlock(void) {
    if (z_mq_mutex_ready) LightLock_Unlock(&z_mq_mutex);
}

static void pc_mq_wait(void) {
    svcSleepThread(1000000LL);
}
#else
#include <SDL.h>
static SDL_mutex* z_mq_mutex = NULL;

void pc_audio_mq_init(void) {
    if (!z_mq_mutex) z_mq_mutex = SDL_CreateMutex();
}

void pc_audio_mq_shutdown(void) {
    if (z_mq_mutex) {
        SDL_DestroyMutex(z_mq_mutex);
        z_mq_mutex = NULL;
    }
}

static void pc_mq_lock(void) {
    if (z_mq_mutex) SDL_LockMutex(z_mq_mutex);
}

static void pc_mq_unlock(void) {
    if (z_mq_mutex) SDL_UnlockMutex(z_mq_mutex);
}

static void pc_mq_wait(void) {
    SDL_Delay(1);
}
#endif
#endif

extern void Z_osWritebackDCacheAll() {
}

extern void osInvalDCache2(void* src, s32 size) {
#ifndef TARGET_PC
    DCInvalidateRange(src, size);
#endif
    (void)src; (void)size;
}

extern void osWritebackDCache2(void* src, s32 size) {
#ifndef TARGET_PC
    DCStoreRange(src, size);
#endif
    (void)src; (void)size;
}

extern void Z_osCreateMesgQueue(OSMesgQueue* mq, OSMesg* msg, s32 count) {
    mq->msg = msg;
    mq->msgCount = count;
    mq->validCount = 0;
    mq->first = 0;
}

extern s32 Z_osSendMesg(OSMesgQueue* mq, OSMesg msg, s32 flags) {
#ifdef TARGET_PC
    pc_mq_lock();
#endif
    int msgCount = mq->msgCount;
    if (mq->validCount == mq->msgCount) {
#ifdef TARGET_PC
        pc_mq_unlock();
#endif
        return -1;
    }

    int count = mq->first + mq->validCount;

    if (count >= mq->msgCount) {
        count -= mq->msgCount;
    }

    mq->msg[count] = msg;

    mq->validCount++;

#ifdef TARGET_PC
    pc_mq_unlock();
#endif
    return 0;
}

extern s32 Z_osRecvMesg(OSMesgQueue* mq, OSMesg* msg, s32 flags) {
#ifdef TARGET_PC
    pc_mq_lock();
#endif
    if (flags == OS_MESG_BLOCK) {
#ifdef TARGET_PC
        /* On PC with threading, spin-wait with mutex release */
        while (!mq->validCount) {
            pc_mq_unlock();
            pc_mq_wait();
            pc_mq_lock();
        }
#else
        while (!mq->validCount) {};
#endif
    }

    if (mq->validCount == 0) {
        if (msg != NULL) {
            *msg = NULL;
        }
#ifdef TARGET_PC
        pc_mq_unlock();
#endif
        return -1;
    }

    mq->validCount -= 1;

    if (msg != NULL) {
        *msg = mq->msg[mq->first];
    }

    mq->first++;

    if (mq->first == mq->msgCount) {
        mq->first = 0;
    }

#ifdef TARGET_PC
    pc_mq_unlock();
#endif
    return 0;
}

extern s32 Z_osEPiStartDma(OSPiHandle* handler, OSIoMesg* msg, s32 dir) {
    ARAMStartDMAmesg(1, (uintptr_t)msg->dramAddr, msg->devAddr, msg->size, 0, msg->hdr.retQueue);
    return 0;
}

void Z_bcopy(void* src, void* dst, size_t size) {
    Jac_bcopy(src, dst, size);
}
