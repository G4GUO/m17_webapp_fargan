/*
 * c2fargan.h - FARGAN neural synthesizer for the Codec2 3200 bitstream
 *
 * Drop-in alternative to codec2_decode(): it takes the same 8-byte Codec2 3200
 * frames and returns 20 ms of speech at 16 kHz (320 samples) instead of 8 kHz.
 * The Codec2 parameters (pitch, energy, LSPs, voicing) are decoded exactly as
 * codec2_decode() does, then FARGAN synthesizes the waveform from them.
 *
 * Latency: the output lags the classic Codec2 decoder output by 10 ms.
 *
 * LGPL-2.1. The FARGAN runtime in third_party/opus is BSD-3-Clause (Xiph.Org).
 */
#ifndef C2FARGAN_H
#define C2FARGAN_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define C2F_BYTES_PER_FRAME   8     /* Codec2 3200 frame, 20 ms */
#define C2F_SAMPLES_PER_FRAME 320   /* output per frame */
#define C2F_SAMPLE_RATE       16000 /* output sample rate */

typedef struct c2f_decoder c2f_decoder;

/* heap allocation */
c2f_decoder *c2f_create(void);
void c2f_destroy(c2f_decoder *dec);

/* caller-provided memory (embedded use): c2f_size() bytes, suitably aligned */
size_t c2f_size(void);
void c2f_init(c2f_decoder *dec);

/* forget all history - call at the start of every new transmission */
void c2f_reset(c2f_decoder *dec);

/* decode one Codec2 3200 frame (8 bytes) into 320 samples at 16 kHz */
void c2f_decode(c2f_decoder *dec, int16_t *pcm, const uint8_t *bits);
void c2f_decode_float(c2f_decoder *dec, float *pcm, const uint8_t *bits); /* range +-1.0 */

#ifdef __cplusplus
}
#endif

#endif /* C2FARGAN_H */
