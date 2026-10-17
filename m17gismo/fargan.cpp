// fargan.cpp - the FARGAN neural receiver, as an option inside the station.
//
// M17-Project/Codec2-mod-neural (vendored in c2fargan/), Wojciech Kaczmarski
// SP5WWP's neural receiver, replaces only the receiver's synthesis step: the
// same 8-byte Codec2 3200 frames become speech at
// 16 kHz instead of 8 kHz.  The bitstream, the transmitter and every other
// station are untouched, which is what makes it a receive-only option rather
// than a fork of M17.
//
// The audio path here is 8 kHz, so the 320 samples FARGAN produces per frame are
// decimated back to the 160 the rest of the program expects.  That decimation
// measured 0.017 ms per frame - 0.09% of a core - so the choice between this and
// widening the audio path to 16 kHz is about work, not performance.
//
// Measured on this station's Pi 4B: 9.6 ms per 20 ms frame, 48% of one core,
// 2.1x real time.  Harness and raw numbers: m17_neural/RESULTS.md.
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "fargan.h"

#ifdef FARGAN_RX

#include "c2fargan.h"

#define OUT_SAMPLES C2F_SAMPLES_PER_FRAME   /* 320, 20 ms at 16 kHz */
#define IN_SAMPLES  160                     /* what the audio path wants */

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------
// FARGAN's state is small and c2fargan offers caller-provided memory, so this
// keeps the program's static style instead of allocating: no malloc, no failure
// path at a bad moment.  c2f_size() is checked at first use.
static unsigned char s_storage[8192] __attribute__((aligned(16)));
static c2f_decoder *s_dec;
static bool s_on;
static bool s_ready;

bool fargan_rx_compiled(void) { return true; }
bool fargan_rx_enabled(void)  { return s_ready && s_on; }

// ---------------------------------------------------------------------------
// 16 kHz -> 8 kHz: a 31-tap windowed sinc, with its history kept across frames
// so the decimation has no seams at frame boundaries.
// ---------------------------------------------------------------------------
#define RS_TAPS 31
static float s_taps[RS_TAPS];
static float s_hist[RS_TAPS];
static bool  s_taps_ready;

static void decimator_init(void)
{
    const int    M  = (RS_TAPS - 1) / 2;
    const double fc = 0.225;                 /* 0.45 x the 8 kHz Nyquist */
    double sum = 0.0;

    for (int i = 0; i < RS_TAPS; i++) {
        double x = (double)(i - M);
        double s = (x == 0.0) ? 2.0 * fc : sin(2.0 * M_PI * fc * x) / (M_PI * x);
        double w = 0.54 - 0.46 * cos(2.0 * M_PI * (double)i / (double)(RS_TAPS - 1));
        s_taps[i] = (float)(s * w);
        sum += s_taps[i];
    }
    for (int i = 0; i < RS_TAPS; i++) s_taps[i] = (float)(s_taps[i] / sum);
    memset(s_hist, 0, sizeof(s_hist));
    s_taps_ready = true;
}

static int decimate(const float *in, int n_in, int16_t *out)
{
    int o = 0;

    for (int i = 0; i < n_in; i++) {
        memmove(&s_hist[0], &s_hist[1], (RS_TAPS - 1) * sizeof(s_hist[0]));
        s_hist[RS_TAPS - 1] = in[i];
        if ((i & 1) == 0) {
            double acc = 0.0;
            int v;
            for (int k = 0; k < RS_TAPS; k++) acc += (double)s_taps[k] * (double)s_hist[k];
            v = (int)lrint(acc * 32768.0);          /* FARGAN's samples are +-1.0 */
            if (v > 32767)  v = 32767;
            if (v < -32768) v = -32768;
            out[o++] = (int16_t)v;
        }
    }
    return o;
}

// ---------------------------------------------------------------------------
// Starting a new transmission
// ---------------------------------------------------------------------------
// c2fargan asks for c2f_reset() at the start of every transmission: its output
// lags the classic decoder by 10 ms and its first frame is silent by design.
// This file is called from the receive path with no stream-start indication, so
// an idle gap stands in for one - nothing decoded for a second means the next
// frame is the start of something new.  Cheap, and the worst case is a reset in
// the middle of a long pause.
#define NEW_TX_GAP_S 1.0
static struct timespec s_last;
static bool s_have_last;

static bool new_transmission(void)
{
    struct timespec now;
    double gap;
    bool isnew = true;

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (s_have_last) {
        gap = (double)(now.tv_sec - s_last.tv_sec) + 1e-9 * (double)(now.tv_nsec - s_last.tv_nsec);
        isnew = (gap > NEW_TX_GAP_S);
    }
    s_last = now;
    s_have_last = true;
    return isnew;
}

static bool ensure_decoder(void)
{
    if (s_ready) return true;

    if (c2f_size() > sizeof(s_storage)) {
        printf("FARGAN: needs %lu bytes of state, only %lu available\n",
               (unsigned long)c2f_size(), (unsigned long)sizeof(s_storage));
        return false;
    }
    if (!s_taps_ready) decimator_init();
    s_dec = (c2f_decoder *)s_storage;
    c2f_init(s_dec);
    s_ready = true;
    return true;
}

void fargan_rx_reset(void)
{
    if (s_dec) c2f_reset(s_dec);
    memset(s_hist, 0, sizeof(s_hist));
    s_have_last = false;
}

void fargan_rx_set(bool on)
{
    if (on && !ensure_decoder()) {
        printf("FARGAN: not available\n");
        s_on = false;
        return;
    }
    if (on) fargan_rx_reset();
    s_on = on;
}

void fargan_decode_to_8k(const uint8_t *bits8, int16_t *out160)
{
    float f[OUT_SAMPLES];

    if (!ensure_decoder()) {
        memset(out160, 0, IN_SAMPLES * sizeof(int16_t));
        return;
    }
    if (new_transmission()) c2f_reset(s_dec);
    c2f_decode_float(s_dec, f, bits8);
    if (decimate(f, OUT_SAMPLES, out160) != IN_SAMPLES)
        memset(out160, 0, IN_SAMPLES * sizeof(int16_t));
}

void fargan_decode_to_16k(const uint8_t *bits8, int16_t *out320)
{
    if (!ensure_decoder()) {
        memset(out320, 0, OUT_SAMPLES * sizeof(int16_t));
        return;
    }
    if (new_transmission()) c2f_reset(s_dec);
    c2f_decode(s_dec, out320, bits8);
}

#else  // !FARGAN_RX - built without the neural receiver (make NEURAL=0)

bool fargan_rx_compiled(void) { return false; }
bool fargan_rx_enabled(void)  { return false; }
void fargan_rx_set(bool on)   { (void)on; }
void fargan_rx_reset(void)    {}

void fargan_decode_to_8k(const uint8_t *bits8, int16_t *out160)
{
    (void)bits8;
    memset(out160, 0, 160 * sizeof(int16_t));
}

void fargan_decode_to_16k(const uint8_t *bits8, int16_t *out320)
{
    (void)bits8;
    memset(out320, 0, 320 * sizeof(int16_t));
}

#endif // FARGAN_RX
