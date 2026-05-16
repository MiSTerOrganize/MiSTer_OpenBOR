/*
 * MiSTer_OpenBOR_4086 -- sdl/sblaster.c MiSTer replacement
 *
 * Option C v4 (4086-specific): engine at upstream native 44.1 kHz, glue
 * layer resamples to 48 kHz via ZERO-ORDER HOLD (sample-and-hold /
 * nearest-neighbor) — matches what PC SDL 1.2's default resampler does.
 *
 * Per src/audio/SDL_audiocvt.c in libsdl-org/SDL-1.2: the SDL_RateMUL*
 * upsamplers duplicate samples (dst[0] = src[0]; dst[1] = src[0];) and
 * the SDL_RateDIV* downsamplers drop samples (dst[0] = src[0]; src += 2;).
 * No interpolation. PC OpenBOR build 4086 links against SDL 1.2.15 and
 * its audio path goes through these same RateMUL/DIV functions.
 *
 * For PC reference parity on MiSTer, we mirror SDL 1.2's zero-order hold.
 * This preserves the "crunchy" nearest-neighbor character of PC OpenBOR
 * build 4086 audio exactly — not a higher-quality variant, the actual
 * platform behavior. Sister core 7533 ships polyphase windowed-sinc to
 * match its SDL2 PC reference; the SDL version is what diverges.
 *
 * SDL-version → resampler mapping (load-bearing rule for hybrid cores):
 *   SDL 1.2   → zero-order hold (sample duplication / nearest-neighbor)
 *   SDL 2     → polyphase windowed-sinc FIR (bandlimited interpolation)
 *
 * Implementation rules:
 *   - uint32_t accum (always positive — no negative-shift UB)
 *   - No cross-tick state (each tick self-contained, accum starts 0)
 *   - STEP shift via uint64_t intermediate (avoids int32 overflow at
 *     rate >= 32768 — the 2026-05-15 "loud buzzing" trap)
 *
 * Copyright (C) 2026 MiSTer Organize -- GPL-3.0
 */

#include "sblaster.h"
#include "soundmix.h"
#include "sdlport.h"
#include "native_audio_writer.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* OpenBOR's mixer renders stereo S16 PCM at 44.1 kHz upstream native. */
extern void update_sample(unsigned char *buf, int size);

#define ENGINE_AUDIO_RATE    44100
#define MISTER_AUDIO_RATE    48000
#define MISTER_AUDIO_CHUNK   256                      /* output frames per tick (48 kHz)   */
#define MISTER_CHUNK_BYTES   (MISTER_AUDIO_CHUNK * 4) /* stereo S16                          */

/* 256 output × 44100/48000 = 235.2 input frames needed per tick. Request
 * 236 (ceil) so the last src index (~235) stays in-bounds. */
#define IN_FRAMES_PER_TICK   236

static int              started;
static int              voicevol = 15;
static pthread_t        audio_thread;
static volatile int     audio_thread_run;

static void audio_sleep_us(long us) {
    struct timespec ts;
    ts.tv_sec  = us / 1000000L;
    ts.tv_nsec = (us % 1000000L) * 1000L;
    nanosleep(&ts, NULL);
}

static void *audio_thread_fn(void *arg) {
    (void)arg;
    static int16_t in_buf[IN_FRAMES_PER_TICK * 2];   /* stereo S16 @ 44.1 kHz from engine */
    static int16_t out_buf[MISTER_AUDIO_CHUNK * 2];  /* stereo S16 @ 48 kHz for DDR3      */

    /* 16.16 step per output sample: (44100 << 16) / 48000 = 60211.
     * Cast to uint64_t before shift to avoid the int32 overflow trap. */
    const uint32_t STEP = (uint32_t)(((uint64_t)ENGINE_AUDIO_RATE << 16) / MISTER_AUDIO_RATE);

    /* Soft-limiter state — envelope-following stereo-linked design (mirrored
     * from MiSTer_OpenBOR_7533). Applied after the zero-order-hold resample
     * to catch multi-voice mixer sums that exceed [-32768, 32767]. Same
     * limiter design across both cores even though Stage 2 method differs
     * (4086 = zero-order hold per SDL 1.2 PC reference; 7533 = polyphase
     * windowed-sinc per SDL 2 PC reference). */
    const float ATTACK_COEFF  = expf(-1.0f / (0.001f * (float)MISTER_AUDIO_RATE));
    const float RELEASE_COEFF = expf(-1.0f / (0.100f * (float)MISTER_AUDIO_RATE));
    const float THRESHOLD     = 27852.0f;  /* -1.5 dBFS */
    float lim_env  = 0.0f;
    float lim_gain = 1.0f;

    while (audio_thread_run) {
        size_t free_frames = NativeAudioWriter_FreeFrames();

        if (free_frames < (size_t)MISTER_AUDIO_CHUNK) {
            audio_sleep_us(3000);
            continue;
        }

        update_sample((unsigned char *)in_buf, IN_FRAMES_PER_TICK * 4);

        /* Zero-order hold 44.1 → 48 kHz + soft-limiter, per stereo frame.
         * Zero-order hold matches SDL 1.2's SDL_RateMUL behavior. */
        uint32_t accum = 0;
        int i;
        for (i = 0; i < MISTER_AUDIO_CHUNK; i++) {
            int ip = (int)(accum >> 16);
            if (ip >= IN_FRAMES_PER_TICK) ip = IN_FRAMES_PER_TICK - 1;

            int16_t l = in_buf[2 * ip + 0];
            int16_t r = in_buf[2 * ip + 1];

            /* Soft-limiter: envelope follower + smoothed stereo-linked gain. */
            float L = (float)l, R = (float)r;
            float aL = L < 0.0f ? -L : L;
            float aR = R < 0.0f ? -R : R;
            float peak = aL > aR ? aL : aR;

            if (peak > lim_env) lim_env = peak;
            else                lim_env = lim_env * RELEASE_COEFF + peak * (1.0f - RELEASE_COEFF);

            float target_gain = (lim_env > THRESHOLD) ? (THRESHOLD / lim_env) : 1.0f;

            if (target_gain < lim_gain)
                lim_gain = lim_gain * ATTACK_COEFF + target_gain * (1.0f - ATTACK_COEFF);
            else
                lim_gain = lim_gain * RELEASE_COEFF + target_gain * (1.0f - RELEASE_COEFF);

            int Lo = (int)(L * lim_gain);
            int Ro = (int)(R * lim_gain);
            if (Lo > 32767)  Lo = 32767;
            if (Lo < -32768) Lo = -32768;
            if (Ro > 32767)  Ro = 32767;
            if (Ro < -32768) Ro = -32768;

            out_buf[2 * i + 0] = (int16_t)Lo;
            out_buf[2 * i + 1] = (int16_t)Ro;

            accum += STEP;
        }

        NativeAudioWriter_Submit(out_buf, MISTER_AUDIO_CHUNK);
    }
    return NULL;
}

int SB_playstart(int bits, int samplerate) {
    (void)bits;
    (void)samplerate;

    if (started) return 1;

    if (!NativeAudioWriter_IsActive()) {
        return 0;
    }

    audio_thread_run = 1;
    if (pthread_create(&audio_thread, NULL, audio_thread_fn, NULL) != 0) {
        audio_thread_run = 0;
        return 0;
    }
    started = 1;
    return 1;
}

void SB_playstop(void) {
    if (!started) return;
    audio_thread_run = 0;
    pthread_join(audio_thread, NULL);
    started = 0;
}

void SB_setvolume(char dev, char volume) {
    if (dev == SB_VOICEVOL) voicevol = volume;
}

void SB_updatevolume(int volume) {
    voicevol += volume;
    if (voicevol > 15) voicevol = 15;
    if (voicevol < 0)  voicevol = 0;
}
