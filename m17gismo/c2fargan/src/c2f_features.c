/*
 * c2f_features.c - Codec2 3200 frame -> FARGAN conditioning features
 *
 * Feature row (first 20 of the 36 floats FARGAN was trained with):
 *   0      loudness, 0.3 * (10 log10(e) - 20)
 *   1..10  decoded LSPs, deviation from uniform spacing, x4
 *   11     voicing bit (0/1)
 *   12..17 LPC cepstrum c1..c6 of the decoded all-pole envelope
 *   18     pitch, log2(256 / P16) - 1.5, P16 = period at 16 kHz, 32..254
 *   19     voicing as a pitch correlation proxy (+0.5 / -0.3)
 *
 * The parameter decoding mirrors codec2_decode() in Codec2-mod. Any change
 * here must also go into training/c2feat.c, otherwise the model sees
 * features it was not trained on.
 *
 * LGPL-2.1 (derived from Codec2 / Codec2-mod).
 */
#include <string.h>
#include <math.h>
#include "codec2_mod.h"
#include "quantise.h"
#include "interp.h"
#include "lpc.h"
#include "c2f_features.h"

void c2f_params_init(c2f_params_state *st)
{
    /* same initial state as codec2_decoder_init() */
    st->prev_Wo = TWO_PI / P_MAX;
    st->prev_L = (int)(M_PI / st->prev_Wo);
    st->prev_voiced = 0;
    st->prev_e = 1.0f;
    for (int i = 0; i < LPC_ORD; i++)
        st->prev_lsps[i] = i * M_PI / (LPC_ORD + 1);
}

static void lpc_cepstrum(const float *ak, float *c, int n)
{
    /* ak[0] = 1, A(z) = sum ak[k] z^-k, cepstrum of 1/A(z) */
    for (int m = 1; m <= n; m++) {
        double acc = (m <= LPC_ORD) ? -ak[m] : 0.0;
        for (int k = 1; k < m; k++)
            if (m - k <= LPC_ORD) acc -= ((double)k / m) * c[k - 1] * ak[m - k];
        c[m - 1] = (float)acc;
    }
}

static void make_row(float *row, float e, const float *lsp, int voiced, float Wo)
{
    float ak[LPC_ORD + 1], cep[6];
    lsp_to_lpc(lsp, ak);
    lpc_cepstrum(ak, cep, 6);

    row[0] = 0.3f * (10.0f * log10f(e > 1e-3f ? e : 1e-3f) - 20.0f);
    for (int i = 0; i < LPC_ORD; i++)
        row[1 + i] = 4.0f * (lsp[i] - (i + 1) * (float)M_PI / (LPC_ORD + 1));
    row[11] = voiced ? 1.0f : 0.0f;
    for (int i = 0; i < 6; i++) row[12 + i] = cep[i];
    float p16 = 2.0f * TWO_PI / Wo;     /* pitch period in 16 kHz samples */
    if (p16 < 32.0f) p16 = 32.0f;
    if (p16 > 254.0f) p16 = 254.0f;     /* FARGAN needs period <= 254 */
    row[18] = log2f(256.0f / p16) - 1.5f;
    row[19] = voiced ? 0.5f : -0.3f;
}

void c2f_frame_features(c2f_params_state *st, float feat[2][C2F_NB_FEATURES], const uint8_t *bits)
{
    model_t model[2], prev;
    float lsps[2][LPC_ORD], e[2];
    int idx[LPC_ORD];
    unsigned int nbit = 0;

    memset(model, 0, sizeof(model));
    memset(&prev, 0, sizeof(prev));
    prev.Wo = st->prev_Wo;
    prev.L = st->prev_L;
    prev.voiced = st->prev_voiced;

    /* same unpack and interpolation as codec2_decode() */
    model[0].voiced = unpack(bits, &nbit, 1);
    model[1].voiced = unpack(bits, &nbit, 1);
    model[1].Wo = decode_Wo(unpack(bits, &nbit, WO_BITS), WO_BITS);
    model[1].L = M_PI / model[1].Wo;
    e[1] = decode_energy(unpack(bits, &nbit, E_BITS), E_BITS);
    for (int i = 0; i < LSPD_SCALAR_INDEXES; i++) idx[i] = unpack(bits, &nbit, 5);
    decode_lspds_scalar(&lsps[1][0], idx);
    interp_Wo(&model[0], &prev, &model[1]);
    e[0] = interp_energy(st->prev_e, e[1]);
    interpolate_lsp(&lsps[0][0], st->prev_lsps, &lsps[1][0]);

    for (int i = 0; i < 2; i++)
        make_row(feat[i], e[i], lsps[i], model[i].voiced, model[i].Wo);

    st->prev_Wo = model[1].Wo;
    st->prev_L = model[1].L;
    st->prev_voiced = model[1].voiced;
    st->prev_e = e[1];
    memcpy(st->prev_lsps, lsps[1], sizeof(st->prev_lsps));
}
