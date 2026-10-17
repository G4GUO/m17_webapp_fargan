/*
 * c2f_features.h - Codec2 3200 frame -> FARGAN conditioning features
 *
 * Decodes the parameters of one frame exactly like codec2_decode() in
 * Codec2-mod (unpack, dequantise, interpolate to 10 ms) and maps them to two
 * rows of FARGAN features. Must match training/c2feat.c bit for bit.
 *
 * LGPL-2.1 (derived from Codec2 / Codec2-mod).
 */
#ifndef C2F_FEATURES_H
#define C2F_FEATURES_H

#include <stdint.h>

#define C2F_NB_FEATURES 20   /* FARGAN uses the first 20 of 36 */

typedef struct {
    float prev_Wo;
    int prev_L;
    int prev_voiced;
    float prev_e;
    float prev_lsps[10];
} c2f_params_state;

void c2f_params_init(c2f_params_state *st);

/* one 8-byte frame -> feat[0] (first 10 ms) and feat[1] (second 10 ms) */
void c2f_frame_features(c2f_params_state *st, float feat[2][C2F_NB_FEATURES], const uint8_t *bits);

#endif
