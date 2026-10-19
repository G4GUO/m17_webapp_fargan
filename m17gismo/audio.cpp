// audio.cpp
//
// Audio dispatcher: the M17 code calls audio_input()/audio_output(); this
// module decides which of the compiled back ends actually carries the audio.
//
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#include "m17defines.h"
#include "audio.h"

static AudioBackend m_backend = AUDIO_BROWSER;
static bool m_mic_open;
static bool m_spk_open;
static AudioBackend m_pending = AUDIO_NONE;   // requested while open

static double m_mic_level;
static double m_spk_level;

// ---------------------------------------------------------------------------
static bool want_pulse(AudioBackend b) {
    return b == AUDIO_PULSE || b == AUDIO_BOTH;
}
static bool want_browser(AudioBackend b) {
    return b == AUDIO_BROWSER || b == AUDIO_BOTH;
}
static bool want_file(AudioBackend b) {
    return b == AUDIO_FILE;
}
static bool want_alsa(AudioBackend b) {
    return b == AUDIO_ALSA;
}

bool audio_set_backend(AudioBackend b) {
    if (b == AUDIO_NONE) {
        // Nothing to do: silence in, discard out
    }
    m_backend = b;
    if (m_mic_open || m_spk_open) {
        m_pending = b;   // applied on the next close
    }
    return true;
}

AudioBackend audio_parse_backend(const char *name) {
    if (!name) return AUDIO_NONE;
    if (strcasecmp(name, "alsa") == 0 || strcasecmp(name, "local") == 0) return AUDIO_ALSA;
    if (strcasecmp(name, "pulse") == 0) return AUDIO_PULSE;
    if (strcasecmp(name, "browser") == 0 || strcasecmp(name, "web") == 0) return AUDIO_BROWSER;
    if (strcasecmp(name, "file") == 0) return AUDIO_FILE;
    if (strcasecmp(name, "both") == 0) return AUDIO_BOTH;
    if (strcasecmp(name, "none") == 0) return AUDIO_NONE;
    return AUDIO_NONE;
}

const char *audio_backend_name(AudioBackend b) {
    switch (b) {
        case AUDIO_ALSA:    return "alsa";
        case AUDIO_PULSE:   return "pulse";
        case AUDIO_BROWSER: return "browser";
        case AUDIO_FILE:    return "file";
        case AUDIO_BOTH:    return "both";
        default:            return "none";
    }
}

AudioBackend audio_get_backend(void) { return m_backend; }

// A change of device can need more than one attempt: a Bluetooth headset's SCO
// link is created when the first of its two streams opens, so the other one has
// to be tried again once that has happened.
static int m_retry_left;              // attempts left
static struct timespec m_retry_at;    // ...and not before this

// A change was requested while the streams were open: it is applied the next
// time they are closed/opened (the M17 code does that per transmit/receive
// cycle), so the UI can say so and the radio loop can apply it promptly.
bool audio_backend_pending(void) {
    if (m_pending == AUDIO_NONE) return false;
#ifdef HAVE_ALSA
    // Selecting the device that is already open is not a change: re-opening it
    // would only risk losing the stream.  "Nothing is open" is different - that
    // is a device that has not come up yet, and it has to keep being tried.
    bool missing = audio_alsa_side_missing();
    if (!missing && m_pending == AUDIO_ALSA && m_backend == AUDIO_ALSA &&
        !audio_alsa_device_changed()) {
        bool mic = false, spk = false;
        audio_local_streams(&mic, &spk);
        if (mic || spk) return false;
    }
    // A device that was not up yet - a Bluetooth headset's SCO link is created
    // when the first of its streams opens, so the other one has to wait for it -
    // gets a few more attempts, spaced out.
    if (m_retry_left <= 0) {
        // The burst is spent.  A device asked for by name is still worth a slow
        // retry: a speaker that is switched on twenty minutes later has to be
        // picked up without restarting the station, and with no sound card there
        // is nothing else to fall back to.
        if (!audio_alsa_device_requested() && !missing) return false;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - m_retry_at.tv_sec < 15) return false;
        m_retry_at = now;
        return true;
    }
    {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec < m_retry_at.tv_sec) return false;
        m_retry_left--;
        m_retry_at = now;
        m_retry_at.tv_sec += 2;
    }
#endif
    return true;
}

void audio_request_reopen(void) {
    // Always mark it, even with nothing open.  The interesting case is a device
    // that has *not* opened yet - a Bluetooth headset that is not connected, or
    // a card that has been unplugged and put back - and with no stream open there
    // was no close/open cycle to hang the change on, so the station used to sit
    // on a dead device until the next restart.
    m_pending = m_backend;
    m_retry_left = 8;                // a Bluetooth profile can take a moment
    clock_gettime(CLOCK_MONOTONIC, &m_retry_at);
}

// Apply a requested change.  Only the direction that is actually missing or
// moved is touched: closing both to retry one is what made a working speaker
// and a failed microphone take turns succeeding.
void audio_apply_pending(void) {
    if (m_pending != AUDIO_NONE) { m_backend = m_pending; m_pending = AUDIO_NONE; }
#ifdef HAVE_ALSA
    if (!want_alsa(m_backend)) return;
    if (audio_alsa_device_changed()) {
        audio_mic_close(); audio_mic_open();
        audio_spk_close(); audio_spk_open();
        return;
    }
    if (audio_alsa_in_missing())  { audio_mic_close(); audio_mic_open(); }
    if (audio_alsa_out_missing()) { audio_spk_close(); audio_spk_open(); }
#endif
}

bool audio_have_pulse(void) {
#ifdef HAVE_PULSE
    return true;
#else
    return false;
#endif
}

bool audio_have_alsa(void) {
#ifdef HAVE_ALSA
    return true;
#else
    return false;
#endif
}

bool audio_have_browser_audio(void) { return true; }

bool audio_local_ready(void) {
#ifdef HAVE_ALSA
    if (want_alsa(m_backend)) return audio_alsa_ready();
#endif
#ifdef HAVE_PULSE
    if (want_pulse(m_backend)) return audio_pulse_ready();
#endif
    return false;
}

const char *audio_local_device(void) {
#ifdef HAVE_ALSA
    if (want_alsa(m_backend)) return audio_alsa_device();
#endif
    return "";
}

// Which of the local sound card streams are open right now (the ALSA back end
// knows exactly; the others report their own readiness).
void audio_local_streams(bool *mic, bool *spk) {
    bool m = false, s = false;
#ifdef HAVE_ALSA
    if (want_alsa(m_backend)) { m = audio_alsa_mic_is_open(); s = audio_alsa_spk_is_open(); }
#endif
    if (want_pulse(m_backend)) {
#ifdef HAVE_PULSE
        m = s = audio_pulse_ready();
#endif
    }
    if (want_file(m_backend)) m = s = true;
    if (mic) *mic = m;
    if (spk) *spk = s;
}

long audio_tx_dropped(void) {
    long n = 0;
#ifdef HAVE_ALSA
    if (want_alsa(m_backend)) n += audio_alsa_starved();
#endif
    if (want_browser(m_backend)) n += audio_web_mic_starved();
    return n;
}

long audio_tx_overruns(void) {
#ifdef HAVE_ALSA
    if (want_alsa(m_backend)) return audio_alsa_overruns();
#endif
    return 0;
}

const char *audio_backend_error(void) {
#ifdef HAVE_ALSA
    if (want_alsa(m_backend) && audio_alsa_error()[0])
        return audio_alsa_error();
#endif
#ifdef HAVE_PULSE
    if (want_pulse(m_backend) && audio_pulse_error()[0])
        return audio_pulse_error();
#endif
    return "";
}

void audio_backend_status(char *out, int len) {
    snprintf(out, len, "%s", audio_backend_name(m_backend));
}

// ---------------------------------------------------------------------------
void audio_mic_open(void) {
    audio_pace_reset();
#ifdef HAVE_ALSA
    if (want_alsa(m_backend)) audio_alsa_mic_open();
#endif
#ifdef HAVE_PULSE
    if (want_pulse(m_backend)) audio_pulse_mic_open();
#endif
    if (want_browser(m_backend)) audio_web_mic_open();
    if (want_file(m_backend))    audio_file_mic_open();
    m_mic_open = true;
    // If the device asked for was not there (a Bluetooth device still coming
    // up), leave the request standing so the next cycle tries again.
    bool fell = false;
#ifdef HAVE_ALSA
    if (want_alsa(m_backend)) fell = audio_alsa_fell_back();
#endif
    if (fell) { m_pending = m_backend; if (m_retry_left <= 0) m_retry_left = 8; }
    else      m_pending = AUDIO_NONE;
}

void audio_mic_close(void) {
#ifdef HAVE_ALSA
    audio_alsa_mic_close();
#endif
#ifdef HAVE_PULSE
    audio_pulse_mic_close();
#endif
    audio_web_mic_close();
    audio_file_mic_close();
    m_mic_open = false;
    if (m_pending != AUDIO_NONE) { m_backend = m_pending; m_pending = AUDIO_NONE; }
}

void audio_spk_open(void) {
#ifdef HAVE_ALSA
    if (want_alsa(m_backend)) audio_alsa_spk_open();
#endif
#ifdef HAVE_PULSE
    if (want_pulse(m_backend)) audio_pulse_spk_open();
#endif
    if (want_browser(m_backend)) audio_web_spk_open();
    if (want_file(m_backend))    audio_file_spk_open();
    m_spk_open = true;
    bool fell = false;
#ifdef HAVE_ALSA
    if (want_alsa(m_backend)) fell = audio_alsa_fell_back();
#endif
    if (fell) { m_pending = m_backend; if (m_retry_left <= 0) m_retry_left = 8; }
    else      m_pending = AUDIO_NONE;
}

void audio_spk_close(void) {
#ifdef HAVE_ALSA
    audio_alsa_spk_close();
#endif
#ifdef HAVE_PULSE
    audio_pulse_spk_close();
#endif
    audio_web_spk_close();
    audio_file_spk_close();
    m_spk_open = false;
    if (m_pending != AUDIO_NONE) { m_backend = m_pending; m_pending = AUDIO_NONE; }
}

// ---------------------------------------------------------------------------
// The M17 transmit path expects one 20 ms codec frame per call and the radio is
// fed straight from those frames, so the transmit loop has to consume audio in
// real time.
//
// This paces against a running deadline rather than sleeping for whatever is
// left of the current block: the work between two calls - codec2 encode, M17
// modulation, the radio push - costs real milliseconds on a Pi, and sleeping a
// full block on top of that made the loop consume only ~60% of real time.  A
// transmitter running slow starves the far end and overflows the microphone
// ring, which is what "dropping transmit samples" sounds like.
//
// If the radio blocks (the Pluto's push does), the deadline simply falls behind
// and is resynced, so this never double-throttles.
static struct timespec m_pace_next;
static bool m_pace_valid;

void audio_pace_reset(void) { m_pace_valid = false; }

static void audio_pace(int len) {
    if (len <= 0) return;
    long add_ns = (long)len * 1000000000L / 8000;
    struct timespec now, want;

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!m_pace_valid) { m_pace_next = now; m_pace_valid = true; }

    m_pace_next.tv_nsec += add_ns;
    while (m_pace_next.tv_nsec >= 1000000000L) { m_pace_next.tv_sec++; m_pace_next.tv_nsec -= 1000000000L; }

    if (m_pace_next.tv_sec < now.tv_sec ||
        (m_pace_next.tv_sec == now.tv_sec && m_pace_next.tv_nsec <= now.tv_nsec)) {
        // Behind: something else (the radio, or a stalled source) is the real
        // clock now, so start the schedule again from here.
        m_pace_next = now;
        return;
    }
    want.tv_sec  = m_pace_next.tv_sec - now.tv_sec;
    want.tv_nsec = m_pace_next.tv_nsec - now.tv_nsec;
    if (want.tv_nsec < 0) { want.tv_sec--; want.tv_nsec += 1000000000L; }
    nanosleep(&want, NULL);
}

static void level_update(double *acc, const int16_t *s, int len) {
    if (len <= 0) return;
    double sum = 0;
    for (int i = 0; i < len; i++) {
        double v = s[i] / 32768.0;
        sum += v * v;
    }
    *acc = sqrt(sum / len);
    if (*acc > 1.0) *acc = 1.0;
}

void audio_input(int16_t *s, int len) {
    struct timespec t1;
    (void)t1;
#ifdef M17WEB_DIAG
    static int ni = 0;
    if ((ni++ % 200) == 0)
        fprintf(stdout, "diag-audio: in  len=%d mic_open=%d backend=%s\n", len, m_mic_open,
                audio_backend_name(m_backend)), fflush(stdout);
#endif
    memset(s, 0, sizeof(int16_t) * len);
    if (!m_mic_open) return;

    if (want_browser(m_backend)) {
        // Preferred source when a browser is streaming; falls through to
        // PulseAudio/file when nobody is.
        if (audio_web_input(s, len) > 0) {
            level_update(&m_mic_level, s, len);
            audio_pace(len);
            return;
        }
    }
    if (want_file(m_backend)) {
        audio_file_input(s, len);
        level_update(&m_mic_level, s, len);
        audio_pace(len);
        return;
    }
#ifdef HAVE_ALSA
    if (want_alsa(m_backend)) {
        audio_alsa_input(s, len);
        level_update(&m_mic_level, s, len);
        audio_pace(len);
        return;
    }
#endif
#ifdef HAVE_PULSE
    if (want_pulse(m_backend)) {
        audio_pulse_input(s, len);
        level_update(&m_mic_level, s, len);
        audio_pace(len);
        return;
    }
#endif
    m_mic_level = 0;
    audio_pace(len);
}

void audio_output(int16_t *s, int len) {
#ifdef M17WEB_DIAG
    static int no = 0;
    if ((no++ % 50) == 0)
        fprintf(stdout, "diag-audio: out len=%d spk_open=%d backend=%s first=%d\n", len, m_spk_open,
                audio_backend_name(m_backend), (s && len > 0) ? s[0] : 0), fflush(stdout);
#endif
    if (len <= 0) return;
    level_update(&m_spk_level, s, len);
    if (!m_spk_open) return;
#ifdef HAVE_ALSA
    if (want_alsa(m_backend)) audio_alsa_output(s, len);
#endif
#ifdef HAVE_PULSE
    if (want_pulse(m_backend)) audio_pulse_output(s, len);
#endif
    if (want_browser(m_backend)) audio_web_output(s, len);
    if (want_file(m_backend))    audio_file_output(s, len);
}

// Drop microphone audio captured before the transmitter was ready.  Called at
// the start of an over: without it the buffered pre-transmission audio is sent
// first and the whole over lags the microphone by the ring's depth.
void audio_input_flush(void) {
#ifdef HAVE_ALSA
    if (want_alsa(m_backend)) audio_alsa_flush();
#endif
#ifdef HAVE_PULSE
    audio_pulse_flush();
#endif
    if (want_browser(m_backend)) audio_web_mic_flush();
    if (want_file(m_backend)) audio_file_mic_close();
}

void audio_output_flush(void) {
#ifdef HAVE_PULSE
    audio_pulse_flush();
#endif
}

double audio_get_mic_level(void) { return m_mic_level; }
double audio_get_spk_level(void) { return m_spk_level; }
