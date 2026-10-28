// audio.cpp
//
// Audio dispatcher.  The M17 code calls audio_input()/audio_output(); this
// module decides which of the back ends actually carries the audio.
//
// The speaker and the microphone are routed *separately*, each to one of
//
//   mute       nothing at all
//   browser    the web page, over the WebSocket (audio_web.cpp)
//   soundcard  the Pi's sound card, direct through ALSA (audio_alsa.cpp)
//   bluetooth  the device chosen in "Configure Bluetooth", which bluealsa
//              presents to ALSA as an ordinary PCM
//
// which is what the interface's Audio configuration box selects.  The older
// whole-pair back ends (alsa, browser, both, none, pulse, file) are still
// accepted and simply map onto the two routes, so "-a alsa", "audio both" and
// the file back end used by the tests go on working.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#include "m17defines.h"
#include "audio.h"
#include "bluetooth.h"

static AudioRoute m_r_out = AROUTE_BROWSER;   // what was asked for

// Both web routes carry audio over the WebSocket and differ only in which client
// it goes to; every "is this the browser back end" test means both.
static bool route_web(AudioRoute r) { return r == AROUTE_BROWSER || r == AROUTE_PHONE; }

static AudioRoute m_r_in  = AROUTE_BROWSER;
static AudioRoute m_open_out = AROUTE_MUTE;   // what is actually attached
static AudioRoute m_open_in  = AROUTE_MUTE;
static bool m_file;                           // -a file:... (test path)
static char m_card_dev[128];                  // a card named by hand: "audio alsa plughw:3,0"
static bool m_mic_open;
static bool m_spk_open;
static bool m_change;                        // a change is waiting for a safe moment

static double m_mic_level;
static double m_spk_level;
// When each level was last measured.  The meters are driven from these values,
// and nothing updates them when the audio stops - an over ends, or the station
// stops talking - so without a timestamp the needle sat where the last frame
// left it.  That is the "meter does not go back to zero" that gets reported.
static struct timespec m_mic_level_at;
static struct timespec m_spk_level_at;

// Level controls, applied here on the Pi rather than in the browser: the gain
// sits between the microphone and the modulator, and the volume between the
// decoder and the speaker, so the meters measure the audio that is really being
// transmitted / played.
static double m_mic_gain = 1.0;
static double m_spk_vol  = 0.8;

// ---------------------------------------------------------------------------
// Routes
// ---------------------------------------------------------------------------
const char *audio_route_name(AudioRoute r) {
    switch (r) {
        case AROUTE_BROWSER: return "browser";
        case AROUTE_PHONE:   return "phone";
        case AROUTE_CARD:    return "soundcard";
        case AROUTE_BT:      return "bluetooth";
        case AROUTE_PULSE:   return "pulse";
        default:             return "mute";
    }
}

AudioRoute audio_parse_route(const char *name) {
    if (!name || !name[0]) return AROUTE_MUTE;
    if (strcasecmp(name, "browser") == 0 || strcasecmp(name, "web") == 0)  return AROUTE_BROWSER;
    if (strcasecmp(name, "phone") == 0 || strcasecmp(name, "app") == 0 ||
        strcasecmp(name, "android") == 0)                                    return AROUTE_PHONE;
    if (strcasecmp(name, "soundcard") == 0 || strcasecmp(name, "card") == 0 ||
        strcasecmp(name, "alsa") == 0 || strcasecmp(name, "local") == 0)   return AROUTE_CARD;
    if (strcasecmp(name, "bluetooth") == 0 || strcasecmp(name, "bt") == 0)  return AROUTE_BT;
    if (strcasecmp(name, "pulse") == 0 || strcasecmp(name, "pulseaudio") == 0) return AROUTE_PULSE;
    if (strcasecmp(name, "mute") == 0 || strcasecmp(name, "none") == 0 ||
        strcasecmp(name, "off") == 0)                                     return AROUTE_MUTE;
    return AROUTE_MUTE;
}

AudioRoute audio_get_route(bool out) { return out ? m_r_out : m_r_in; }

void audio_route_summary(char *out, int len) {
    if (!out || len <= 0) return;
    if (m_file) { snprintf(out, (size_t)len, "file"); return; }
    if (m_r_out == m_r_in) { snprintf(out, (size_t)len, "%s", audio_route_name(m_r_out)); return; }
    snprintf(out, (size_t)len, "%s / %s", audio_route_name(m_r_out), audio_route_name(m_r_in));
}

// Point the ALSA back end at the device each direction asks for.  A Bluetooth
// direction is a bluealsa PCM; the sound card direction is whatever card is
// present, and for capture that has to be resolved explicitly, because leaving
// the capture device empty makes it follow the *playback* device - which is the
// Bluetooth speaker when the two directions are on different things.
static void sync_devices(void) {
#ifdef HAVE_ALSA
    char play[160], cap[160];
    play[0] = cap[0] = 0;
    bt_alsa_devices(play, sizeof(play), cap, sizeof(cap));

    if (m_r_out == AROUTE_BT) {
        if (play[0]) audio_alsa_set_device(play);
        // No device chosen: leave the request alone and let the open fall back
        // to the card, saying so, rather than going silent.
    } else if (m_r_out == AROUTE_CARD) {
        audio_alsa_set_device(m_card_dev);      // "" = whatever card is present
    }

    if (m_r_in == AROUTE_BT) {
        if (cap[0]) audio_alsa_set_capture_device(cap);
    } else if (m_r_in == AROUTE_CARD) {
        if (m_card_dev[0]) audio_alsa_set_capture_device(m_card_dev);
        else               audio_alsa_capture_follow_card();
    }
#endif
}

void audio_apply_routes(AudioRoute out, AudioRoute in) {
    m_r_out = out;
    m_r_in  = in;
    sync_devices();
    audio_request_reopen();
}

// The card the "soundcard" route should use, by name.  Kept apart from the
// routes so a Bluetooth selection does not overwrite it, and so naming a device
// survives the next route change - it used to be forgotten the moment anything
// called audio_apply_routes() again.
void audio_set_card_device(const char *dev) {
    snprintf(m_card_dev, sizeof(m_card_dev), "%s", dev ? dev : "");
}

void audio_set_route(bool out, AudioRoute r) {
    if (out) audio_apply_routes(r, m_r_in);
    else     audio_apply_routes(m_r_out, r);
}

// ---------------------------------------------------------------------------
// The whole-pair back ends, kept for -a and the console
// ---------------------------------------------------------------------------
bool audio_set_backend(AudioBackend b) {
    switch (b) {
        case AUDIO_ALSA:    m_file = false; audio_apply_routes(AROUTE_CARD, AROUTE_CARD); break;
        case AUDIO_PULSE:   m_file = false; audio_apply_routes(AROUTE_PULSE, AROUTE_PULSE); break;
        case AUDIO_BROWSER: m_file = false; audio_apply_routes(AROUTE_BROWSER, AROUTE_BROWSER); break;
        case AUDIO_BOTH:    m_file = false; audio_apply_routes(AROUTE_CARD, AROUTE_BROWSER); break;
        case AUDIO_FILE:    m_file = true;  break;   // the routes stay as configured; the file carries it
        case AUDIO_NONE:
        default:            m_file = false; audio_apply_routes(AROUTE_MUTE, AROUTE_MUTE); break;
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

// The pair, derived from the two routes: what the state reports and what the
// old "audio" command shows.
AudioBackend audio_get_backend(void) {
    if (m_file) return AUDIO_FILE;
    AudioRoute o = m_r_out, i = m_r_in;
    if (o == AROUTE_PULSE || i == AROUTE_PULSE) return AUDIO_PULSE;
    if (o == AROUTE_MUTE && i == AROUTE_MUTE) return AUDIO_NONE;
    if (route_web(o) && route_web(i)) return AUDIO_BROWSER;
    if (o == AROUTE_CARD && route_web(i)) return AUDIO_BOTH;
    return AUDIO_ALSA;      // card or Bluetooth: the ALSA back end carries it
}

static bool use_alsa_out(void) { return m_r_out == AROUTE_CARD || m_r_out == AROUTE_BT; }
static bool use_alsa_in(void)  { return m_r_in  == AROUTE_CARD || m_r_in  == AROUTE_BT; }

// A change of device can need more than one attempt: a Bluetooth headset's SCO
// link is created when the first of its two streams opens, so the other one has
// to be tried again once that has happened.
static int m_retry_left;              // attempts left
static struct timespec m_retry_at;    // ...and not before this

// A change was requested while the streams were open: it is applied the next
// time they are closed/opened (the M17 code does that per transmit/receive
// cycle), so the UI can say so and the radio loop can apply it promptly.
bool audio_backend_pending(void) {
    // "Pending" means there is something to do before the audio is next used,
    // so it is only ever true while a stream is open.  A station whose mode has
    // no local audio at all - gate bridges the radio to a reflector - never
    // opens one, and must not sit there claiming to be applying a change.
    bool open_any = m_mic_open || m_spk_open;
    if (!open_any) return false;
    if (m_change) return true;
    if (m_open_in != m_r_in || m_open_out != m_r_out) return true;
#ifdef HAVE_ALSA
    if (!use_alsa_in() && !use_alsa_out()) return false;
    // A Bluetooth direction that is selected but not open - the SCO link may not
    // exist yet - is worth retrying.  A card that is simply not on the bus is
    // not: there is nothing to wait for, and retrying fills the log for ever.
    if (!audio_alsa_out_missing() && !audio_alsa_in_missing())
        return audio_alsa_device_changed();
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (m_retry_left > 0) {                 // a burst, a couple of seconds apart
        if (now.tv_sec < m_retry_at.tv_sec) return false;
        m_retry_left--;
        m_retry_at = now;
        m_retry_at.tv_sec += 2;
        return true;
    }
    // Then slowly, for ever: a speaker switched on twenty minutes later has to
    // be picked up without restarting the station.
    if (now.tv_sec - m_retry_at.tv_sec < 15) return false;
    m_retry_at = now;
    return true;
#else
    return false;
#endif
}

void audio_request_reopen(void) {
    // Mark it when there is a stream to close and open again.  With nothing open
    // there is no cycle to hang the change on - the next open simply uses the
    // new route - but the retry burst is set anyway, so a device that was asked
    // for by name and is not there yet is tried again shortly.
    if (m_mic_open || m_spk_open) m_change = true;
    m_retry_left = 8;                // a Bluetooth profile can take a moment
    clock_gettime(CLOCK_MONOTONIC, &m_retry_at);
}

static void audio_arm_retry(void) {
    m_retry_left = 8;
    clock_gettime(CLOCK_MONOTONIC, &m_retry_at);
}

// Apply a requested change.  Only the direction that is actually missing or
// moved is touched: closing both to retry one is what made a working speaker
// and a failed microphone take turns succeeding.
void audio_apply_pending(void) {
    bool in_changed  = (m_open_in  != m_r_in);
    bool out_changed = (m_open_out != m_r_out);
    bool in_missing = false, out_missing = false, in_moved = false, out_moved = false;
#ifdef HAVE_ALSA
    in_missing  = audio_alsa_in_missing();
    out_missing = audio_alsa_out_missing();
    // The device behind a route can change without the route changing - picking
    // a different Bluetooth headset for the microphone, say.  The old stream has
    // to be released before the new one opens, or it stays attached and the new
    // device never gets used, which looks like the selection being ignored.
    in_moved  = audio_alsa_in_device_changed();
    out_moved = audio_alsa_out_device_changed();
#endif
    if (in_changed || in_missing || in_moved)   { audio_mic_close(); audio_mic_open(); }
    if (out_changed || out_missing || out_moved) { audio_spk_close(); audio_spk_open(); }
}

// ---------------------------------------------------------------------------
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
    if (use_alsa_out() || use_alsa_in()) return audio_alsa_ready();
#endif
#ifdef HAVE_PULSE
    if (m_r_out == AROUTE_PULSE || m_r_in == AROUTE_PULSE) return audio_pulse_ready();
#endif
    return false;
}

const char *audio_local_device(void) {
#ifdef HAVE_ALSA
    if (use_alsa_out() || use_alsa_in()) return audio_alsa_device();
#endif
    return "";
}

// Which of the local sound card streams are open right now (the ALSA back end
// knows exactly; the others report their own readiness).
void audio_local_streams(bool *mic, bool *spk) {
    bool m = false, s = false;
    if (m_file) { m = s = true; }
#ifdef HAVE_ALSA
    if (use_alsa_in())  m = audio_alsa_mic_is_open();
    if (use_alsa_out()) s = audio_alsa_spk_is_open();
#endif
#ifdef HAVE_PULSE
    if (m_r_in == AROUTE_PULSE || m_r_out == AROUTE_PULSE) {
        bool r = audio_pulse_ready();
        if (m_r_in  == AROUTE_PULSE) m = r;
        if (m_r_out == AROUTE_PULSE) s = r;
    }
#endif
    if (mic) *mic = m;
    if (spk) *spk = s;
}

long audio_tx_dropped(void) {
    long n = 0;
#ifdef HAVE_ALSA
    if (use_alsa_in()) n += audio_alsa_starved();
#endif
    if (route_web(m_r_in)) n += audio_web_mic_starved();
    return n;
}

long audio_tx_overruns(void) {
#ifdef HAVE_ALSA
    if (use_alsa_in()) return audio_alsa_overruns();
#endif
    return 0;
}

const char *audio_backend_error(void) {
#ifdef HAVE_ALSA
    if ((use_alsa_in() || use_alsa_out()) && audio_alsa_error()[0])
        return audio_alsa_error();
#endif
#ifdef HAVE_PULSE
    if ((m_r_in == AROUTE_PULSE || m_r_out == AROUTE_PULSE) && audio_pulse_error()[0])
        return audio_pulse_error();
#endif
    return "";
}

void audio_backend_status(char *out, int len) {
    audio_route_summary(out, len);
}

// ---------------------------------------------------------------------------
void audio_mic_open(void) {
    audio_pace_reset();
    m_mic_open = true;

    if (m_file) {
        audio_file_mic_open();
        m_open_in = m_r_in;             // the file is the carrier: nothing pending
        return;
    }

    switch (m_r_in) {
        case AROUTE_BROWSER:
        case AROUTE_PHONE:   audio_web_mic_open(); break;
        case AROUTE_CARD:
        case AROUTE_BT:
#ifdef HAVE_ALSA
            audio_alsa_mic_open();
#endif
            break;
        case AROUTE_PULSE:
#ifdef HAVE_PULSE
            audio_pulse_mic_open();
#endif
            break;
        default: break;                 // mute: silence in
    }
    m_open_in = m_r_in;

    // If the device asked for was not there (a Bluetooth device still coming
    // up), leave the request standing so the next cycle tries again.
    bool fell = false, named = false;
#ifdef HAVE_ALSA
    if (use_alsa_in()) fell = audio_alsa_fell_back();
    named = audio_alsa_in_missing() || audio_alsa_out_missing();
#endif
    // A device that is named but not up yet is retried on the cadence in
    // audio_backend_pending().  Do NOT ask for an immediate reopen here: the
    // failure would be its own trigger and the station would hammer bluealsa in
    // a tight loop, filling the log several times a second.
    if (fell && named) audio_arm_retry();
    else if (m_open_in == m_r_in && m_open_out == m_r_out) m_change = false;
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
    m_open_in = AROUTE_MUTE;
}

void audio_spk_open(void) {
    m_spk_open = true;

    if (m_file) {
        audio_file_spk_open();
        m_open_out = m_r_out;
        return;
    }

    switch (m_r_out) {
        case AROUTE_BROWSER:
        case AROUTE_PHONE:   audio_web_spk_open(); break;
        case AROUTE_CARD:
        case AROUTE_BT:
#ifdef HAVE_ALSA
            audio_alsa_spk_open();
#endif
            break;
        case AROUTE_PULSE:
#ifdef HAVE_PULSE
            audio_pulse_spk_open();
#endif
            break;
        default: break;                 // mute: discard
    }
    m_open_out = m_r_out;

    bool fell = false, named = false;
#ifdef HAVE_ALSA
    if (use_alsa_out()) fell = audio_alsa_fell_back();
    named = audio_alsa_in_missing() || audio_alsa_out_missing();
#endif
    // A device that is named but not up yet is retried on the cadence in
    // audio_backend_pending().  Do NOT ask for an immediate reopen here: the
    // failure would be its own trigger and the station would hammer bluealsa in
    // a tight loop, filling the log several times a second.
    if (fell && named) audio_arm_retry();
    else if (m_open_in == m_r_in && m_open_out == m_r_out) m_change = false;
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
    m_open_out = AROUTE_MUTE;
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

static void level_update(double *acc, struct timespec *at, const int16_t *s, int len) {
    if (len <= 0) return;
    double sum = 0;
    for (int i = 0; i < len; i++) {
        double v = s[i] / 32768.0;
        sum += v * v;
    }
    *acc = sqrt(sum / len);
    if (*acc > 1.0) *acc = 1.0;
    clock_gettime(CLOCK_MONOTONIC, at);
}

// A meter falls back when it stops being driven: a short hold, so a gap between
// syllables is not a reading, then a fast decay - about what a real movement
// does, and enough that the needle is at rest a second or two after the audio
// stops instead of hanging at the last frame's level.
static double level_read(double level, const struct timespec *at) {
    if (level <= 0.0) return 0.0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double dt = (double)(now.tv_sec - at->tv_sec) + (now.tv_nsec - at->tv_nsec) / 1e9;
    if (dt <= 0.30) return level;              // 300 ms hold
    double db = 20.0 * log10(level) - 40.0 * (dt - 0.30);
    if (db <= -70.0) return 0.0;
    return pow(10.0, db / 20.0);
}

// The microphone gain, applied to the samples about to be encoded: this is the
// point the meter is taken from, so the VU meter reads the level the modulator
// gets rather than a level the browser happened to see.
static void apply_gain(int16_t *s, int len, double gain) {
    if (gain == 1.0 || len <= 0) return;
    for (int i = 0; i < len; i++) {
        double v = s[i] * gain;
        if (v >  32767.0) v =  32767.0;
        if (v < -32768.0) v = -32768.0;
        s[i] = (int16_t)(v < 0 ? v - 0.5 : v + 0.5);
    }
}

void audio_input(int16_t *s, int len) {
    if (len <= 0) return;
#ifdef M17WEB_DIAG
    static int ni = 0;
    if ((ni++ % 200) == 0)
        fprintf(stdout, "diag-audio: in  len=%d mic_open=%d routes=%s/%s\n", len, m_mic_open,
                audio_route_name(m_r_in), audio_route_name(m_r_out)), fflush(stdout);
#endif
    memset(s, 0, sizeof(int16_t) * (size_t)len);
    if (!m_mic_open) return;

    if (m_file) {
        audio_file_input(s, len);
        apply_gain(s, len, m_mic_gain);
        level_update(&m_mic_level, &m_mic_level_at, s, len);
        audio_pace(len);
        return;
    }

    switch (m_r_in) {
        case AROUTE_BROWSER:
        case AROUTE_PHONE:
            audio_web_input(s, len);
            break;
        case AROUTE_CARD:
        case AROUTE_BT:
#ifdef HAVE_ALSA
            audio_alsa_input(s, len);
#endif
            break;
        case AROUTE_PULSE:
#ifdef HAVE_PULSE
            audio_pulse_input(s, len);
#endif
            break;
        default: break;                     // mute: stays silent
    }
    apply_gain(s, len, m_mic_gain);
    level_update(&m_mic_level, &m_mic_level_at, s, len);
    audio_pace(len);
}

void audio_output(int16_t *s, int len) {
    if (len <= 0) return;
#ifdef M17WEB_DIAG
    static int no = 0;
    if ((no++ % 50) == 0)
        fprintf(stdout, "diag-audio: out len=%d spk_open=%d routes=%s/%s first=%d\n", len, m_spk_open,
                audio_route_name(m_r_out), audio_route_name(m_r_in), s[0]), fflush(stdout);
#endif
    double vol = m_spk_vol;
    apply_gain(s, len, vol);
    level_update(&m_spk_level, &m_spk_level_at, s, len);
    if (!m_spk_open) return;

    if (m_file) { audio_file_output(s, len); return; }

    switch (m_r_out) {
        case AROUTE_BROWSER:
        case AROUTE_PHONE:   audio_web_output(s, len); break;
        case AROUTE_CARD:
        case AROUTE_BT:
#ifdef HAVE_ALSA
            audio_alsa_output(s, len);
#endif
            break;
        case AROUTE_PULSE:
#ifdef HAVE_PULSE
            audio_pulse_output(s, len);
#endif
            break;
        default: break;                     // mute: discarded
    }
}

// Drop microphone audio captured before the transmitter was ready.  Called at
// the start of an over: without it the buffered pre-transmission audio is sent
// first and the whole over lags the microphone by the ring's depth.
void audio_input_flush(void) {
#ifdef HAVE_ALSA
    if (use_alsa_in() || use_alsa_out()) audio_alsa_flush();
#endif
#ifdef HAVE_PULSE
    audio_pulse_flush();
#endif
    audio_web_mic_flush();
    if (m_file) audio_file_mic_close();
}

void audio_output_flush(void) {
#ifdef HAVE_PULSE
    audio_pulse_flush();
#endif
}

double audio_get_mic_level(void) { return level_read(m_mic_level, &m_mic_level_at); }
double audio_get_spk_level(void) { return level_read(m_spk_level, &m_spk_level_at); }

// ---------------------------------------------------------------------------
void audio_set_mic_gain(double g) {
    if (g < 0) g = 0;
    if (g > 4) g = 4;
    m_mic_gain = g;
}

double audio_get_mic_gain(void) { return m_mic_gain; }

void audio_set_spk_vol(double v) {
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    m_spk_vol = v;
}

double audio_get_spk_vol(void) { return m_spk_vol; }
