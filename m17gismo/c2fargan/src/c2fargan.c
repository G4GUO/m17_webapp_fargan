/*
 * c2fargan.c - Codec2 3200 frames -> FARGAN -> 16 kHz speech
 *
 * Timing: FARGAN synthesizes the 10 ms slot k-1 when it is given feature row k
 * (one row of look-ahead, as in training). Every Codec2 frame m carries rows
 * 2m and 2m+1, so its output covers slots 2m-1 and 2m. That is 10 ms behind
 * the classic Codec2 decoder. The very first slot of a transmission is silence.
 *
 * LGPL-2.1. Uses the FARGAN runtime from Opus (third_party/opus, BSD-3-Clause).
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "c2fargan.h"
#include "c2f_features.h"
#include "fargan.h"

struct c2f_decoder {
    FARGANState fargan;
    c2f_params_state params;
    int started;
};

size_t c2f_size(void) { return sizeof(c2f_decoder); }

void c2f_reset(c2f_decoder *dec)
{
    fargan_init(&dec->fargan);
    c2f_params_init(&dec->params);
    dec->started = 0;
}

void c2f_init(c2f_decoder *dec)
{
    memset(dec, 0, sizeof(*dec));
    c2f_reset(dec);
}

c2f_decoder *c2f_create(void)
{
    c2f_decoder *dec = malloc(sizeof(*dec));
    if (dec) c2f_init(dec);
    return dec;
}

void c2f_destroy(c2f_decoder *dec) { free(dec); }

void c2f_decode_float(c2f_decoder *dec, float *pcm, const uint8_t *bits)
{
    float feat[2][C2F_NB_FEATURES];
    c2f_frame_features(&dec->params, feat, bits);

    if (!dec->started) {
        /* prime FARGAN with the first row, the way Opus' fargan_demo does */
        static const float zeros[FARGAN_CONT_SAMPLES];
        float init[5 * NB_FEATURES];
        for (int i = 0; i < 5; i++) memcpy(&init[i * NB_FEATURES], feat[0], sizeof(feat[0]));
        fargan_cont(&dec->fargan, zeros, init);
        memset(pcm, 0, FARGAN_FRAME_SIZE * sizeof(float));
        dec->started = 1;
    } else {
        fargan_synthesize(&dec->fargan, pcm, feat[0]);
    }
    fargan_synthesize(&dec->fargan, pcm + FARGAN_FRAME_SIZE, feat[1]);
}

void c2f_decode(c2f_decoder *dec, int16_t *pcm, const uint8_t *bits)
{
    float f[C2F_SAMPLES_PER_FRAME];
    c2f_decode_float(dec, f, bits);
    for (int i = 0; i < C2F_SAMPLES_PER_FRAME; i++) {
        float s = floorf(0.5f + 32768.0f * f[i]);
        pcm[i] = (int16_t)(s > 32767.0f ? 32767.0f : (s < -32767.0f ? -32767.0f : s));
    }
}
