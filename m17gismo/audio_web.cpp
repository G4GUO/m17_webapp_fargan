// audio_web.cpp
//
// Browser audio back end.  Microphone PCM arrives from the WebSocket client
// (see webui.cpp) and is buffered here; decoded receive audio is handed to
// webui_audio_to_browser() which frames it for the connected browsers.
//
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

#include "m17defines.h"
#include "audio.h"

#define MIC_RING    (8000 * 2)      // 2 seconds of 8 kHz audio
#define MIC_TIMEOUT_MS 400
// Lead-in before the modulator starts taking microphone audio over the network.
// The audio arrives in 20 ms messages over Wi-Fi, which jitters: without a
// reserve the first late message is a silent gap in the transmission, and a
// blocking read here stalls the 40 ms frame cadence of the transmitter.
#define MIC_PREROLL_SAMPLES (8000 / 8)     // 125 ms
#define MIC_START_ANYWAY_MS 400            // a short over still starts if the client is slow

static pthread_mutex_t m_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  m_cv  = PTHREAD_COND_INITIALIZER;

static int16_t m_ring[MIC_RING];
static int m_head, m_tail, m_count;      // ring buffer of mic samples
static bool m_mic_active;
static bool m_spk_active;
static bool m_preroll_done;              // this over has enough audio in hand
static long m_over_start_ms;             // when this over's first block was asked for
static long m_mic_frames;                // samples pushed by the browser
static long m_spk_frames;                // samples sent to the browser
static long m_mic_starved;               // samples the transmitter had to pad with silence

// ---------------------------------------------------------------------------
void audio_web_mic_open(void) {
    pthread_mutex_lock(&m_mtx);
    m_head = m_tail = m_count = 0;
    m_mic_active = true;
    m_mic_frames = 0;
    m_preroll_done = false;
    m_over_start_ms = 0;
    pthread_mutex_unlock(&m_mtx);
}

void audio_web_mic_close(void) {
    pthread_mutex_lock(&m_mtx);
    m_mic_active = false;
    m_head = m_tail = m_count = 0;
    pthread_mutex_unlock(&m_mtx);
}

void audio_web_spk_open(void) {
    pthread_mutex_lock(&m_mtx);
    m_spk_active = true;
    m_spk_frames = 0;
    pthread_mutex_unlock(&m_mtx);
}

void audio_web_spk_close(void) {
    pthread_mutex_lock(&m_mtx);
    m_spk_active = false;
    pthread_mutex_unlock(&m_mtx);
}

bool audio_web_active(void) {
    bool r;
    pthread_mutex_lock(&m_mtx);
    r = m_mic_active || m_spk_active;
    pthread_mutex_unlock(&m_mtx);
    return r;
}

// ---------------------------------------------------------------------------
// Called from the WebSocket thread with microphone audio
void audio_web_mic_push(const int16_t *s, int len) {
    if (len <= 0) return;
    pthread_mutex_lock(&m_mtx);
    if (!m_mic_active) {
        pthread_mutex_unlock(&m_mtx);
        return;
    }
    for (int i = 0; i < len; i++) {
        if (m_count >= MIC_RING) {
            // Overrun: drop the oldest sample so we stay real time
            m_tail = (m_tail + 1) % MIC_RING;
            m_count--;
        }
        m_ring[m_head] = s[i];
        m_head = (m_head + 1) % MIC_RING;
        m_count++;
    }
    m_mic_frames += len;
    pthread_cond_signal(&m_cv);
    pthread_mutex_unlock(&m_mtx);
}

int audio_web_mic_available(void) {
    int n;
    pthread_mutex_lock(&m_mtx);
    n = m_count;
    pthread_mutex_unlock(&m_mtx);
    return n;
}

// Blocks until len samples are available (or the timeout expires)
static long mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)(t.tv_sec * 1000L + t.tv_nsec / 1000000L);
}

// Never blocks.  The transmitter asks for a 20 ms block every 20 ms and cannot
// afford to wait for the network: if the audio is not here yet the block is
// silence, and the 125 ms of reserve taken at the start of the over is what
// keeps ordinary Wi-Fi jitter from becoming a hole in the audio.
int audio_web_input(int16_t *s, int len) {
    int got = 0;
    pthread_mutex_lock(&m_mtx);
    if (m_mic_active) {
        if (m_over_start_ms == 0) m_over_start_ms = mono_ms();
        if (!m_preroll_done &&
            (m_count >= MIC_PREROLL_SAMPLES || mono_ms() - m_over_start_ms >= MIC_START_ANYWAY_MS)) {
            m_preroll_done = true;
        }
        if (m_preroll_done) {
            while (got < len && m_count > 0) {
                s[got++] = m_ring[m_tail];
                m_tail = (m_tail + 1) % MIC_RING;
                m_count--;
            }
        }
        if (got < len) {
            // Counted only once the over has really started: the lead-in is
            // deliberate, not a starvation.
            if (m_preroll_done) m_mic_starved += len - got;
            memset(&s[got], 0, sizeof(int16_t) * (size_t)(len - got));
        }
    } else {
        memset(s, 0, sizeof(int16_t) * (size_t)len);
    }
    pthread_mutex_unlock(&m_mtx);
    return got;
}

void audio_web_output(int16_t *s, int len) {
    if (len <= 0) return;
    pthread_mutex_lock(&m_mtx);
    bool active = m_spk_active;
    m_spk_frames += len;
    pthread_mutex_unlock(&m_mtx);
    if (active) webui_audio_to_browser(s, len);
}

long audio_web_mic_starved(void) {
    long n;
    pthread_mutex_lock(&m_mtx);
    n = m_mic_starved;
    pthread_mutex_unlock(&m_mtx);
    return n;
}

// Called at the start of an over: everything buffered is from before the
// transmitter was ready, so it goes, and the drop count starts from here.
void audio_web_mic_flush(void) {
    pthread_mutex_lock(&m_mtx);
    m_head = m_tail = m_count = 0;
    m_mic_starved = 0;
    m_preroll_done = false;
    m_over_start_ms = 0;
    pthread_mutex_unlock(&m_mtx);
}

long audio_web_mic_frames(void) {
    long n;
    pthread_mutex_lock(&m_mtx);
    n = m_mic_frames;
    pthread_mutex_unlock(&m_mtx);
    return n;
}
