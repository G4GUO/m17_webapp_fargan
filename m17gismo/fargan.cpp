// fargan.cpp - the FARGAN neural receiver, as an option inside the station.
//
// M17-Project/Codec2-mod-neural (vendored in c2fargan/), Wojciech Kaczmarski
// SP5WWP's neural receiver, replaces only the receiver's synthesis step: the
// same 8-byte Codec2 3200 frames become speech at 16 kHz instead of 8 kHz.  The
// bitstream, the transmitter and every other station are untouched, which is
// what makes this a receive-only option rather than a fork of M17.
//
// The synthesis costs about 9.6 ms for every 20 ms frame - half a Pi 4 core -
// and the receive path runs on the radio thread and on the reflector thread,
// neither of which can spare it: the demodulator would fall behind and audio
// would be dropped.  So the work happens here, on its own thread:
//
//   receive thread --push frame--> [frame queue] --> synthesis thread
//                                                          |
//   receive thread <--pull block-- [block queue] <---------+  16 kHz -> 8 kHz
//
// one frame of latency (20 ms) in exchange for the receive path never waiting.
// The station's audio path is 8 kHz, so FARGAN's 320 samples are decimated to
// 160 here, on the synthesis thread, where the filter's history has one owner.
//
// Measured on this station's Pi 4B: 9.6 ms per 20 ms frame, 2.1x real time.
// Harness and raw numbers: m17_neural/RESULTS.md.
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <atomic>

#include "fargan.h"

#ifdef FARGAN_RX

#include "c2fargan.h"

#define IN_SAMPLES  160                     /* what the audio path wants */
#define OUT_SAMPLES C2F_SAMPLES_PER_FRAME   /* 320, 20 ms at 16 kHz */

// ---------------------------------------------------------------------------
// The decoder and the two queues
// ---------------------------------------------------------------------------
// FARGAN's state is small and c2fargan offers caller-provided memory, so this
// keeps the program's static style instead of allocating.  Only the synthesis
// thread ever touches it.
static unsigned char s_storage[8192] __attribute__((aligned(16)));
static c2f_decoder *s_dec;

// The station has two receive threads (radio and reflector) and both push and
// pull, so the short critical sections are taken under a mutex rather than left
// to a lock-free scheme that would assume one producer and one consumer.
#define Q_FRAMES 32
#define Q_BLOCKS 32

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static uint8_t s_fq[Q_FRAMES][C2F_BYTES_PER_FRAME];
static unsigned s_fq_head, s_fq_tail;

// Each finished frame is kept at both rates: the 8 kHz the audio path plays,
// and FARGAN's own 16 kHz, which is what a recording should keep.  Decimating
// here rather than in the receive path keeps the filter history on one thread.
typedef struct {
    int16_t pcm8[IN_SAMPLES];
    int16_t pcm16[OUT_SAMPLES];
} rx_block;

static rx_block s_bq[Q_BLOCKS];
static unsigned s_bq_head, s_bq_tail;

static std::atomic<bool>     s_on{false};        /* the option is switched on */
static std::atomic<bool>     s_wanted{false};    /* the thread should live    */
static std::atomic<unsigned> s_reset_req{0};     /* reset at the next frame   */
static pthread_t             s_thread;
static bool                  s_thread_started = false;

static unsigned long s_queued, s_decoded, s_dropped, s_dry;

bool fargan_rx_compiled(void) { return true; }
bool fargan_rx_enabled(void)  { return s_on.load(); }

void fargan_rx_stats(unsigned long *queued, unsigned long *decoded,
                     unsigned long *dropped, unsigned long *dry)
{
    pthread_mutex_lock(&s_lock);
    if (queued)  *queued  = s_queued;
    if (decoded) *decoded = s_decoded;
    if (dropped) *dropped = s_dropped;
    if (dry)     *dry     = s_dry;
    pthread_mutex_unlock(&s_lock);
}

// ---------------------------------------------------------------------------
// 16 kHz -> 8 kHz: a 31-tap windowed sinc, history kept across frames.  Runs on
// the synthesis thread only, so this state has a single owner.
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
        double v = (x == 0.0) ? 2.0 * fc : sin(2.0 * M_PI * fc * x) / (M_PI * x);
        double w = 0.54 - 0.46 * cos(2.0 * M_PI * (double)i / (double)(RS_TAPS - 1));
        s_taps[i] = (float)(v * w);
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
// A new transmission should start with a clean decoder: c2fargan asks for
// c2f_reset() per transmission, its output lags by 10 ms and its first frame is
// silent.  Nothing here is told where a stream starts, so an idle gap stands in
// for one - a second with no frames means the next one is something new.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// The synthesis thread
// ---------------------------------------------------------------------------
static void block_push(const rx_block *block)
{
    pthread_mutex_lock(&s_lock);
    if (s_bq_head - s_bq_tail >= Q_BLOCKS) {
        s_bq_tail++;                 /* the consumer is behind: drop the oldest */
        s_dropped++;
    }
    s_bq[s_bq_head % Q_BLOCKS] = *block;
    s_bq_head++;
    pthread_mutex_unlock(&s_lock);
}

static void *synthesis_thread(void *arg)
{
    (void)arg;

    while (s_wanted.load()) {
        uint8_t frame[C2F_BYTES_PER_FRAME];
        float f[OUT_SAMPLES];
        int16_t block[IN_SAMPLES];
        bool got = false;

        pthread_mutex_lock(&s_lock);
        if (s_fq_tail != s_fq_head) {
            memcpy(frame, s_fq[s_fq_tail % Q_FRAMES], sizeof(frame));
            s_fq_tail++;
            got = true;
        }
        pthread_mutex_unlock(&s_lock);

        if (!got) {
            usleep(500);             /* a quarter of a frame time */
            continue;
        }

        if (s_reset_req.exchange(0)) c2f_reset(s_dec);
        else if (new_transmission()) c2f_reset(s_dec);

        c2f_decode_float(s_dec, f, frame);
        {
            rx_block b;
            for (int i = 0; i < OUT_SAMPLES; i++) {
                int v = (int)lrint(f[i] * 32768.0);
                if (v > 32767)  v = 32767;
                if (v < -32768) v = -32768;
                b.pcm16[i] = (int16_t)v;
            }
            if (decimate(f, OUT_SAMPLES, b.pcm8) == IN_SAMPLES) {
                block_push(&b);
                pthread_mutex_lock(&s_lock);
                s_decoded++;
                pthread_mutex_unlock(&s_lock);
            }
        }
    }
    return NULL;
}

static bool ensure_ready(void)
{
    if (s_thread_started) return true;
    if (c2f_size() > sizeof(s_storage)) {
        printf("FARGAN: needs %lu bytes of state, only %lu available\n",
               (unsigned long)c2f_size(), (unsigned long)sizeof(s_storage));
        return false;
    }
    if (!s_taps_ready) decimator_init();
    s_dec = (c2f_decoder *)s_storage;
    c2f_init(s_dec);
    s_wanted.store(true);
    if (pthread_create(&s_thread, NULL, synthesis_thread, NULL) != 0) {
        printf("FARGAN: could not start the synthesis thread\n");
        s_wanted.store(false);
        return false;
    }
    s_thread_started = true;
    return true;
}

// ---------------------------------------------------------------------------
// Called from the receive threads
// ---------------------------------------------------------------------------
void fargan_rx_set(bool on)
{
    if (on && !ensure_ready()) {
        printf("FARGAN: not available\n");
        s_on.store(false);
        return;
    }
    if (on) {
        // Start clean: no audio from before the switch, and a fresh decoder.
        pthread_mutex_lock(&s_lock);
        s_fq_tail = s_fq_head;
        s_bq_tail = s_bq_head;
        pthread_mutex_unlock(&s_lock);
        s_have_last = false;
        s_reset_req.fetch_add(1);
    }
    s_on.store(on);
}

void fargan_rx_reset(void) { s_reset_req.fetch_add(1); }

void fargan_rx_push(const uint8_t *bits8)
{
    if (!s_on.load()) return;

    pthread_mutex_lock(&s_lock);
    if (s_fq_head - s_fq_tail >= Q_FRAMES) {
        s_fq_tail++;                 /* live audio wins over the backlog */
        s_dropped++;
    }
    memcpy(s_fq[s_fq_head % Q_FRAMES], bits8, C2F_BYTES_PER_FRAME);
    s_fq_head++;
    s_queued++;
    pthread_mutex_unlock(&s_lock);
}

int fargan_rx_pull(int16_t *out8k, int16_t *out16k)
{
    int got = 0;

    pthread_mutex_lock(&s_lock);
    if (s_bq_tail != s_bq_head) {
        const rx_block *b = &s_bq[s_bq_tail % Q_BLOCKS];
        if (out8k)  memcpy(out8k,  b->pcm8,  IN_SAMPLES * sizeof(int16_t));
        if (out16k) memcpy(out16k, b->pcm16, OUT_SAMPLES * sizeof(int16_t));
        s_bq_tail++;
        got = 1;
    } else {
        s_dry++;
    }
    pthread_mutex_unlock(&s_lock);
    return got;
}

#else  // !FARGAN_RX - built without the neural receiver (make NEURAL=0)

bool fargan_rx_compiled(void) { return false; }
bool fargan_rx_enabled(void)  { return false; }
void fargan_rx_set(bool on)   { (void)on; }
void fargan_rx_reset(void)    {}
void fargan_rx_push(const uint8_t *bits8) { (void)bits8; }
int  fargan_rx_pull(int16_t *out8k, int16_t *out16k)
{
    if (out8k)  memset(out8k,  0, 160 * sizeof(int16_t));
    if (out16k) memset(out16k, 0, 320 * sizeof(int16_t));
    return 0;
}
void fargan_rx_stats(unsigned long *q, unsigned long *d, unsigned long *dr, unsigned long *dy)
{
    if (q) *q = 0; if (d) *d = 0; if (dr) *dr = 0; if (dy) *dy = 0;
}

#endif // FARGAN_RX
