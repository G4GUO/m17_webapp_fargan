// audio_alsa.cpp
//
// ALSA back end: talks to the sound card directly, with no PulseAudio in the
// path.  On this Pi that matters, because Debian's ALSA "default" PCM is routed
// *through* PulseAudio, and PulseAudio can refuse to run a card (it leaves the
// sink suspended with the ALSA device closed while a stream sits attached).
//
// Device selection, in order:
//   1. the device set with "audio alsa <device>" (web UI / config.txt / command)
//   2. the M17WEB_ALSA_DEVICE environment variable
//   3. the first USB/PnP audio card found (that is usually the one you plugged
//      in, and HDMI is never it), as plughw:<card>,0 which handles rate
//      conversion for 8 kHz mono
//   4. "sysdefault" if no USB card is present
//
// Example overrides: "audio alsa plughw:3,0", "audio alsa hw:CARD=Device,DEV=0"
//
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include <alsa/asoundlib.h>

#include "m17defines.h"
#include "audio.h"

#define ALSA_RATE 8000

static snd_pcm_t *m_out;
static snd_pcm_t *m_in;
static char m_requested[128];      // playback device
static char m_requested_cap[128];  // capture device, "" = same as playback
static char m_used_in[128];
static char m_used_out[128];
static char m_error[160];
static int m_error_stream;   // 1 = microphone, 2 = speaker: what m_error refers to
static bool m_fb_mic;        // the last microphone open used the fallback device
static bool m_fb_spk;        // ...and the last speaker open
static struct timespec m_last_read;      // when the transmitter last asked for audio
static bool m_have_read;
static bool m_ctl_valid;    // the card's mixer controls have been read (see below)
static long m_overruns;     // capture overruns: microphone audio the card discarded
static long m_starved;      // capture samples lost (ring overflow, or padded silence)

// Defined with the device selection below, but the setters above need them.
static void resolve_auto_device(char *out, size_t n);
static void resolve_device(char *out, size_t n, bool capture);

//
// Capture runs on its own thread into this ring.  The transmit loop only comes
// back for more audio every 40 ms - and can be away longer than that while the
// radio pushes a block - so reading the card directly from that loop lets its
// ring overrun, and every overrun silently throws away microphone audio.  A
// thread that drains the card continuously and a ring that holds a few seconds
// means the card never sees the application's timing at all.
//
#define CAP_RING  16000     // 2 s of 8 kHz audio
static int16_t m_cap_ring[CAP_RING];
static int m_cap_head, m_cap_tail, m_cap_count;
static pthread_mutex_t m_cap_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  m_cap_cv  = PTHREAD_COND_INITIALIZER;
static pthread_t m_cap_thread;
static volatile bool m_cap_run;

const char *audio_alsa_error(void) {
    if (m_error[0] == 0) return "";
    // The stream may have opened on a later attempt: a failure message for
    // hardware that is running now is worse than no message at all.
    if (m_error_stream == 1 && m_in  != NULL) return "";
    if (m_error_stream == 2 && m_out != NULL) return "";
    return m_error;
}
long audio_alsa_overruns(void) { return m_overruns; }
long audio_alsa_starved(void)  { return m_starved; }

// What is actually open right now, as opposed to the last error string - the
// two can disagree if an attempt failed after a stream had already opened.
bool audio_alsa_mic_is_open(void) { return m_in != NULL; }
bool audio_alsa_spk_is_open(void) { return m_out != NULL; }
// The playback device, which is the one worth showing: a Bluetooth speaker
// takes the output while the microphone stays on the sound card.
const char *audio_alsa_device(void) {
    if (m_used_out[0]) return m_used_out;
    if (m_requested[0]) return m_requested;
    const char *env = getenv("M17WEB_ALSA_DEVICE");
    return (env && env[0]) ? env : "";
}

const char *audio_alsa_capture_device(void) {
    if (m_used_in[0]) return m_used_in;
    if (m_requested_cap[0]) return m_requested_cap;
    return audio_alsa_device();
}

// A device re-selected while it is already open is not a change.  It matters:
// closing and re-opening a Bluetooth PCM in the same breath upsets bluealsa
// ("Device or resource busy"), and the station would fall back to the card for
// no reason at all.
bool audio_alsa_fell_back(void) { return m_fb_mic || m_fb_spk; }

bool audio_alsa_device_requested(void) { return m_requested[0] != 0; }

// Is a *Bluetooth* direction selected but not open?  That is the case that has
// to keep being retried: the SCO link may not exist yet, and the other direction
// being open must not stop this one from being tried again.  A card direction is
// left alone - if the card is not there, retrying it forever would achieve
// nothing.
bool audio_alsa_side_missing(void) {
    return audio_alsa_out_missing() || audio_alsa_in_missing();
}

bool audio_alsa_out_missing(void) {
    return m_requested[0] && strncmp(m_requested, "bluealsa:", 9) == 0 && m_out == NULL;
}

bool audio_alsa_in_missing(void) {
    return m_requested_cap[0] && strncmp(m_requested_cap, "bluealsa:", 9) == 0 && m_in == NULL;
}

bool audio_alsa_device_changed(void) {
    char out[128], cap[128];
    resolve_device(out, sizeof(out), false);
    resolve_device(cap, sizeof(cap), true);
    if (m_out != NULL && m_used_out[0] && strcmp(m_used_out, out) != 0) return true;
    if (m_in  != NULL && m_used_in[0]  && strcmp(m_used_in,  cap) != 0) return true;
    return false;
}

void audio_alsa_set_device(const char *dev) {
    snprintf(m_requested, sizeof(m_requested), "%s", dev ? dev : "");
    m_ctl_valid = false;   // a different card has different controls
    audio_request_reopen();
}

void audio_alsa_set_capture_device(const char *dev) {
    snprintf(m_requested_cap, sizeof(m_requested_cap), "%s", dev ? dev : "");
    audio_request_reopen();
}

// A Bluetooth speaker takes the output while the microphone has to stay on the
// sound card, and the card is not the same device as the playback one.
void audio_alsa_capture_follow_card(void) {
    resolve_auto_device(m_requested_cap, sizeof(m_requested_cap));
    audio_request_reopen();
}

// The first USB/PnP card: usually the one that was plugged in, and never HDMI.
// Both the PCM device and the card's mixer controls have to agree on this.
static int pick_usb_card(void) {
    char best[64] = "";
    int card = -1, found = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        char *nm = NULL;
        if (snd_card_get_longname(card, &nm) == 0 && nm) {
            if (strcasestr(nm, "USB") || strcasestr(nm, "PnP")) {
                snprintf(best, sizeof(best), "plughw:%d,0", card);
                found = card;
            }
            free(nm);
        }
    }
    return found;
}

// What the station falls back to when a chosen device is not there: a
// Bluetooth speaker that is switched off must not leave the station silent.
static void resolve_auto_device(char *out, size_t n) {
    const char *env = getenv("M17WEB_ALSA_DEVICE");
    if (env && env[0]) { snprintf(out, n, "%s", env); return; }
    int card = pick_usb_card();
    if (card >= 0) { snprintf(out, n, "plughw:%d,0", card); return; }
    snprintf(out, n, "sysdefault");
}

// Pick something sane when the user has not said which device to use.  Capture
// has its own choice because a Bluetooth speaker takes the output and leaves
// the microphone on the card; with one configured device the two are the same,
// which is what the station did before Bluetooth existed.
static void resolve_device(char *out, size_t n, bool capture) {
    if (capture && m_requested_cap[0]) { snprintf(out, n, "%s", m_requested_cap); return; }
    if (m_requested[0]) { snprintf(out, n, "%s", m_requested); return; }
    resolve_auto_device(out, n);
}

//
// The card's own hardware controls, as opposed to the volumes the application
// keeps for itself.  It matters because a USB card powers up with whatever its
// controls happen to say - the C-Media headset starts at 29% (-20 dB), which is
// quiet enough to lose received audio entirely - and nothing else on the
// station sets them.  These belong to the card rather than to a stream, so they
// can be set while playback is open.
//
// The values asked for are remembered and re-applied every time the audio is
// opened, so a card that comes back after a USB glitch gets them again.
//
#define MIXER_MAX   8
#define MIXER_NAMEL 24

#define MIXER_KIND_NONE    0
#define MIXER_KIND_PLAY    1
#define MIXER_KIND_CAPTURE 2

typedef struct {
    char name[MIXER_NAMEL];
    int  kind;              // playback or capture
    bool has_switch, on;
    long min, max, value;
} MixerCtl;

static MixerCtl m_ctl[MIXER_MAX];
static int       m_ctl_n;

static char m_want_name[MIXER_MAX][MIXER_NAMEL];
static int  m_want_pct[MIXER_MAX];
static int  m_want_n;

static snd_mixer_t *open_mixer(void) {
    int card = pick_usb_card();
    if (card < 0) return NULL;
    char dev[32];
    snprintf(dev, sizeof(dev), "hw:%d", card);
    snd_mixer_t *m = NULL;
    if (snd_mixer_open(&m, 0) < 0) return NULL;
    if (snd_mixer_attach(m, dev) < 0 ||
        snd_mixer_selem_register(m, NULL, NULL) < 0 ||
        snd_mixer_load(m) < 0) {
        snd_mixer_close(m);
        return NULL;
    }
    return m;
}

// Read the card's controls.  Names come from the card itself, which is what
// makes this work for whatever is plugged in rather than a fixed list.
static void refresh_mixer(void) {
    m_ctl_n = 0;
    snd_mixer_t *m = open_mixer();
    if (!m) return;
    for (snd_mixer_elem_t *e = snd_mixer_first_elem(m);
         e && m_ctl_n < MIXER_MAX; e = snd_mixer_elem_next(e)) {
        if (!snd_mixer_selem_is_active(e)) continue;
        const char *nm = snd_mixer_selem_get_name(e);
        if (!nm || !nm[0]) continue;
        MixerCtl *c = &m_ctl[m_ctl_n];
        memset(c, 0, sizeof(*c));
        snprintf(c->name, sizeof(c->name), "%s", nm);
        c->has_switch = false;
        c->on = true;
        // Capture is tested first on purpose: a control can carry both a
        // playback (monitor) side and a capture side - the USB PnP card's 'Mic'
        // does - and a control named for the microphone is there for the
        // microphone.  Reading the playback side reported 0% and, worse, setting
        // it moved the monitor level instead of the microphone gain.
        if (snd_mixer_selem_has_capture_volume(e)) {
            c->kind = MIXER_KIND_CAPTURE;
            snd_mixer_selem_get_capture_volume_range(e, &c->min, &c->max);
            snd_mixer_selem_get_capture_volume(e, SND_MIXER_SCHN_FRONT_LEFT, &c->value);
            if (snd_mixer_selem_has_capture_switch(e)) {
                int v = 1;
                c->has_switch = true;
                if (snd_mixer_selem_get_capture_switch(e, SND_MIXER_SCHN_FRONT_LEFT, &v) == 0)
                    c->on = (v != 0);
            }
        } else if (snd_mixer_selem_has_playback_volume(e)) {
            c->kind = MIXER_KIND_PLAY;
            snd_mixer_selem_get_playback_volume_range(e, &c->min, &c->max);
            snd_mixer_selem_get_playback_volume(e, SND_MIXER_SCHN_FRONT_LEFT, &c->value);
            if (snd_mixer_selem_has_playback_switch(e)) {
                int v = 1;
                c->has_switch = true;
                if (snd_mixer_selem_get_playback_switch(e, SND_MIXER_SCHN_FRONT_LEFT, &v) == 0)
                    c->on = (v != 0);
            }
        } else {
            // A switch with no level (the card's Auto Gain Control).
            c->kind = MIXER_KIND_PLAY;
            c->min = 0; c->max = 100; c->value = 100;
            if (snd_mixer_selem_has_playback_switch(e)) {
                int v = 1;
                c->has_switch = true;
                if (snd_mixer_selem_get_playback_switch(e, SND_MIXER_SCHN_FRONT_LEFT, &v) == 0)
                    c->on = (v != 0);
            }
        }
        m_ctl_n++;
    }
    snd_mixer_close(m);
}

static void mixer_ctl_refresh_if_needed(void) {
    if (!m_ctl_valid) { refresh_mixer(); m_ctl_valid = true; }
}

int  audio_alsa_mixer_count(void) { mixer_ctl_refresh_if_needed(); return m_ctl_n; }

const char *audio_alsa_mixer_name(int i) {
    mixer_ctl_refresh_if_needed();
    return (i >= 0 && i < m_ctl_n) ? m_ctl[i].name : "";
}
// Current level as a percentage of what the control allows, so the UI can use
// one slider shape for every card.
int audio_alsa_mixer_pct(int i) {
    mixer_ctl_refresh_if_needed();
    if (i < 0 || i >= m_ctl_n) return 0;
    MixerCtl *c = &m_ctl[i];
    if (c->max <= c->min) return c->value > 0 ? 100 : 0;
    return (int)((c->value - c->min) * 100 / (c->max - c->min));
}
bool audio_alsa_mixer_has_switch(int i) {
    mixer_ctl_refresh_if_needed();
    return (i >= 0 && i < m_ctl_n) && m_ctl[i].has_switch;
}
bool audio_alsa_mixer_on(int i) {
    mixer_ctl_refresh_if_needed();
    return (i >= 0 && i < m_ctl_n) ? m_ctl[i].on : false;
}
bool audio_alsa_mixer_is_capture(int i) {
    mixer_ctl_refresh_if_needed();
    return (i >= 0 && i < m_ctl_n) && m_ctl[i].kind == MIXER_KIND_CAPTURE;
}

// Set a control, by the name it reported, to a percentage.
bool audio_alsa_mixer_set(const char *name, int pct) {
    if (!name || !name[0]) return false;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    snd_mixer_t *m = open_mixer();
    if (!m) return false;
    bool done = false;
    for (snd_mixer_elem_t *e = snd_mixer_first_elem(m); e && !done; e = snd_mixer_elem_next(e)) {
        const char *nm = snd_mixer_selem_get_name(e);
        if (!nm || strcmp(nm, name) != 0) continue;
        long min = 0, max = 0;
        // Same preference as the read: capture first, so a 'Mic' control with
        // both sides moves the microphone gain rather than a monitor level.
        if (snd_mixer_selem_has_capture_volume(e)) {
            snd_mixer_selem_get_capture_volume_range(e, &min, &max);
            snd_mixer_selem_set_capture_volume_all(e, min + (max - min) * pct / 100);
            if (snd_mixer_selem_has_capture_switch(e))
                snd_mixer_selem_set_capture_switch_all(e, pct > 0);
            done = true;
        } else if (snd_mixer_selem_has_playback_volume(e)) {
            snd_mixer_selem_get_playback_volume_range(e, &min, &max);
            snd_mixer_selem_set_playback_volume_all(e, min + (max - min) * pct / 100);
            if (snd_mixer_selem_has_playback_switch(e))
                snd_mixer_selem_set_playback_switch_all(e, pct > 0);
            done = true;
        }
    }
    if (m) snd_mixer_close(m);
    if (!done) return false;
    // Remember it, so it is applied again whenever the audio is opened: the
    // card can vanish and come back, and the settings would otherwise be lost.
    for (int i = 0; i < m_want_n; i++) {
        if (strcmp(m_want_name[i], name) == 0) { m_want_pct[i] = pct; m_ctl_valid = false; return true; }
    }
    if (m_want_n < MIXER_MAX) {
        snprintf(m_want_name[m_want_n], MIXER_NAMEL, "%s", name);
        m_want_pct[m_want_n] = pct;
        m_want_n++;
    }
    m_ctl_valid = false;   // re-read, so the state shows what really landed
    return true;
}

// Called when the audio paths open.  Silently does nothing if there is no card.
void audio_alsa_mixer_apply(void) {
    if (m_want_n == 0) return;
    for (int i = 0; i < m_want_n; i++) audio_alsa_mixer_set(m_want_name[i], m_want_pct[i]);
}

static bool open_pcm(snd_pcm_t **pcm, const char *dev, snd_pcm_stream_t dir, const char *what) {
    // A restart can lose a race with the previous instance releasing the card,
    // and a close/open cycle in the radio loop can be a moment too early: retry
    // while the device reports itself busy rather than surfacing a stale error.
    // Open into a local handle: on failure the caller's pointer must keep
    // pointing at whatever was already open.  Assigning through it let a failed
    // retry wipe a working stream, leaving the device open but unreachable.
    snd_pcm_t *p = NULL;
    int rc = 0;
    // 8 attempts, 350 ms apart: about 2.8 s, enough for a USB card to be
    // released or a bluealsa A2DP stream to tear down.  Opening a device while
    // another process is still letting go of it is the common case here - a
    // restart, a device change, Bluetooth coming back - and is not an error.
    for (int attempt = 0; attempt < 8; attempt++) {
        rc = snd_pcm_open(&p, dev, dir, 0);
        if (rc >= 0) break;
        if (rc != -EBUSY && rc != -EAGAIN) break;
        usleep(350000);
    }
    if (rc < 0) {
        snprintf(m_error, sizeof(m_error), "%s (%s): %s", what, dev, snd_strerror(rc));
        m_error_stream = (dir == SND_PCM_STREAM_CAPTURE) ? 1 : 2;
        return false;
    }
    // Adopt the new handle, but never orphan the old one.  If the caller still
    // has a stream open - two threads applying a device change in the same
    // cycle, a re-open that overlapped one - overwriting the pointer leaves the
    // card held by this very process with nothing pointing at it, and every
    // later open of that device then answers "Device or resource busy" against
    // itself, for good.  Closing it first keeps the pointers and the hardware in
    // step, and a leaked open is worth a moment of extra close here.
    if (*pcm && *pcm != p) {
        snd_pcm_close(*pcm);
        *pcm = NULL;
    }
    *pcm = p;
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    if ((rc = snd_pcm_hw_params_any(p, hw)) < 0 ||
        (rc = snd_pcm_hw_params_set_access(*pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (rc = snd_pcm_hw_params_set_format(*pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
        (rc = snd_pcm_hw_params_set_channels(*pcm, hw, 1)) < 0) {
        snprintf(m_error, sizeof(m_error), "%s: parameter setup: %s", what, snd_strerror(rc));
        snd_pcm_close(*pcm);
        *pcm = NULL;
        return false;
    }
    unsigned int rate = ALSA_RATE;
    if ((rc = snd_pcm_hw_params_set_rate_near(*pcm, hw, &rate, 0)) < 0) {
        snprintf(m_error, sizeof(m_error), "%s: %u Hz not usable: %s", what, ALSA_RATE, snd_strerror(rc));
        snd_pcm_close(*pcm);
        *pcm = NULL;
        return false;
    }
    // ~20 ms of audio per period (one codec2 3200 frame) keeps the pacing right.
    // Playback gets ~160 ms of headroom; capture gets twice that, because a slow
    // moment in the transmit loop overflows the card's capture ring and the
    // samples in it are gone - that is dropped transmit audio, not just latency.
    snd_pcm_uframes_t period = 160;
    snd_pcm_hw_params_set_period_size_near(*pcm, hw, &period, 0);
    unsigned int periods = (dir == SND_PCM_STREAM_CAPTURE) ? 16 : 8;
    snd_pcm_hw_params_set_periods_near(*pcm, hw, &periods, 0);
    if ((rc = snd_pcm_hw_params(*pcm, hw)) < 0) {
        snprintf(m_error, sizeof(m_error), "%s: %s", what, snd_strerror(rc));
        snd_pcm_close(*pcm);
        *pcm = NULL;
        return false;
    }
    snd_pcm_prepare(*pcm);
    m_error[0] = 0;
    m_error_stream = 0;
#ifdef M17WEB_DIAG
    {
        unsigned int got_rate = 0, got_ch = 0;
        snd_pcm_uframes_t got_period = 0, got_buf = 0;
        snd_pcm_hw_params_get_rate(hw, &got_rate, 0);
        snd_pcm_hw_params_get_channels(hw, &got_ch);
        snd_pcm_hw_params_get_period_size(hw, &got_period, 0);
        snd_pcm_hw_params_get_buffer_size(hw, &got_buf);
        fprintf(stdout, "diag-cap: %s %s -> rate=%u ch=%u period=%lu buffer=%lu\n",
                what, dev, got_rate, got_ch, (unsigned long)got_period, (unsigned long)got_buf);
        fflush(stdout);
    }
#endif
    m_error[0] = 0;
    return true;
}

static void close_pcm(snd_pcm_t **pcm) {
    if (*pcm) {
        snd_pcm_drain(*pcm);
        snd_pcm_close(*pcm);
        *pcm = NULL;
    }
}

static void *cap_drain_thread(void *arg) {
    int16_t buf[160];
    long produced = 0;
    struct timespec tstart;
    clock_gettime(CLOCK_MONOTONIC, &tstart);
    while (m_cap_run) {
        snd_pcm_sframes_t n = snd_pcm_readi(m_in, buf, 160);
#ifdef M17WEB_DIAG
        static int nr = 0;
        if (n <= 0 || (nr++ % 100) == 0) {
            struct timespec tn;
            clock_gettime(CLOCK_MONOTONIC, &tn);
            double secs = (tn.tv_sec - tstart.tv_sec) + (tn.tv_nsec - tstart.tv_nsec) / 1e9;
            fprintf(stdout, "diag-cap: readi=%ld produced=%ld in %.2fs = %.0f samples/s ring=%d\n",
                    (long)n, produced, secs, secs > 0 ? produced / secs : 0.0, m_cap_count);
            fflush(stdout);
        }
#endif
        produced += (n > 0) ? n : 0;
        if (n == -EPIPE) {
            // Should not happen with this thread draining continuously; keep the
            // counter honest if it does.
            m_overruns++;
            snd_pcm_prepare(m_in);
            continue;
        }
        if (n == -EAGAIN) { usleep(2000); continue; }
        if (n <= 0) { usleep(5000); continue; }
        pthread_mutex_lock(&m_cap_mtx);
        for (int i = 0; i < n; i++) {
            if (m_cap_count >= CAP_RING) {
                // Keep the newest audio and let the oldest go.  Only count it
                // when the transmitter has been reading: the microphone stays
                // open while receiving, and discarding audio nobody asked for
                // is not a dropped transmit sample.
                m_cap_tail = (m_cap_tail + 1) % CAP_RING;
                m_cap_count--;
                if (m_have_read) {
                    struct timespec now;
                    clock_gettime(CLOCK_MONOTONIC, &now);
                    long since = (now.tv_sec - m_last_read.tv_sec) * 1000 +
                                 (now.tv_nsec - m_last_read.tv_nsec) / 1000000;
                    if (since < 1000) m_starved++;
                }
            }
            m_cap_ring[m_cap_head] = buf[i];
            m_cap_head = (m_cap_head + 1) % CAP_RING;
            m_cap_count++;
        }
        pthread_cond_broadcast(&m_cap_cv);
        pthread_mutex_unlock(&m_cap_mtx);
    }
    return arg;
}

void audio_alsa_mic_open(void) {
    char dev[128], want[128];
    m_fb_mic = false;
    resolve_device(dev, sizeof(dev), true);
    snprintf(want, sizeof(want), "%s", dev);
    if (!open_pcm(&m_in, dev, SND_PCM_STREAM_CAPTURE, "microphone")) {
        // A device asked for by name - a Bluetooth headset, or a card that has
        // been unplugged - must not leave the station deaf.  Fall back to
        // whatever card is present, and say so - unless there is no card at all,
        // in which case a Bluetooth-only station is the intended arrangement and
        // the retry loop will pick the device up when it appears.
        char alt[128];
        resolve_auto_device(alt, sizeof(alt));
        if (pick_usb_card() < 0) {
            printf("Audio Input: %s is not available and no sound card is attached\n", dev);
            snprintf(m_error, sizeof(m_error),
                     "microphone (%s): not available, and no sound card is attached", dev);
        } else if (strcmp(alt, dev) != 0 && open_pcm(&m_in, alt, SND_PCM_STREAM_CAPTURE, "microphone")) {
            printf("Audio Input: %s is not available, using %s\n", dev, alt);
            snprintf(dev, sizeof(dev), "%s", alt);
            m_fb_mic = true;
        }
    }
    if (m_in != NULL) {
        snprintf(m_used_in, sizeof(m_used_in), "%s", dev);
        audio_alsa_mixer_apply();
        pthread_mutex_lock(&m_cap_mtx);
        m_cap_head = m_cap_tail = m_cap_count = 0;
        pthread_mutex_unlock(&m_cap_mtx);
        m_have_read = false;
        m_cap_run = true;
#ifdef M17WEB_DIAG
        fprintf(stdout, "diag-cap: opening capture thread on %s\n", dev);
        fflush(stdout);
#endif
        if (pthread_create(&m_cap_thread, NULL, cap_drain_thread, NULL) != 0) {
            m_cap_run = false;
            snprintf(m_error, sizeof(m_error), "microphone (%s): cannot start capture thread", dev);
            snd_pcm_close(m_in);
            m_in = NULL;
        }
    } else {
        // The speaker may be the direction that brought the HFP link up.  Retry
        // this side on its own, without closing the stream that works - tearing
        // both down and reopening is exactly what made the two directions take
        // turns succeeding.
        if (m_in == NULL) {
            printf("Audio Input failed: %s\n", m_error);
            // Mark it like a fallback so the audio layer keeps the change
            // pending: bluealsa creates the SCO link when the first of a
            // headset's two PCMs opens, so the second can fail and succeed a
            // few seconds later.  Without this the microphone was given up on
            // for the rest of the run.
            m_fb_mic = true;
        }
    }
}

void audio_alsa_spk_open(void) {
    char dev[128], want[128];
    m_fb_spk = false;
    resolve_device(dev, sizeof(dev), false);
    snprintf(want, sizeof(want), "%s", dev);
    if (!open_pcm(&m_out, dev, SND_PCM_STREAM_PLAYBACK, "speaker")) {
        // Same as capture: a Bluetooth speaker that is off, or a card that has
        // gone, falls back to whatever is there rather than going silent.  With
        // no card attached there is nothing to fall back to, and the retry loop
        // waits for the Bluetooth device instead of pretending a card exists.
        char alt[128];
        resolve_auto_device(alt, sizeof(alt));
        if (pick_usb_card() < 0) {
            printf("Audio Output: %s is not available and no sound card is attached\n", dev);
            snprintf(m_error, sizeof(m_error),
                     "speaker (%s): not available, and no sound card is attached", dev);
        } else if (strcmp(alt, dev) != 0 && open_pcm(&m_out, alt, SND_PCM_STREAM_PLAYBACK, "speaker")) {
            printf("Audio Output: %s is not available, using %s\n", dev, alt);
            snprintf(dev, sizeof(dev), "%s", alt);
            m_fb_spk = true;
        }
    }
    if (m_out != NULL) {
        snprintf(m_used_out, sizeof(m_used_out), "%s", dev);
        // The card powers up with its own idea of the levels; put them back.
        audio_alsa_mixer_apply();
        // Prefill with silence: starting playback on an empty buffer guarantees
        // an immediate underrun (XRUN) on the first write.
        int16_t silence[160];
        memset(silence, 0, sizeof(silence));
        for (int i = 0; i < 4; i++) snd_pcm_writei(m_out, silence, 160);
        snd_pcm_start(m_out);
    } else {
        if (m_out == NULL) {
            printf("Audio Output failed: %s\n", m_error);
            m_fb_spk = true;
        }
    }
}

void audio_alsa_mic_close(void) {
#ifdef M17WEB_DIAG
    fprintf(stdout, "diag-cap: mic_close (run=%d ring=%d starved=%ld)\n",
            m_cap_run ? 1 : 0, m_cap_count, m_starved);
    fflush(stdout);
#endif
    if (m_cap_run) {
        m_cap_run = false;
        pthread_cond_broadcast(&m_cap_cv);
        pthread_join(m_cap_thread, NULL);
    }
    pthread_mutex_lock(&m_cap_mtx);
    m_cap_head = m_cap_tail = m_cap_count = 0;
    pthread_mutex_unlock(&m_cap_mtx);
    close_pcm(&m_in);
}
void audio_alsa_spk_close(void)  { close_pcm(&m_out); }

bool audio_alsa_ready(void) { return (m_in != NULL) || (m_out != NULL); }

void audio_alsa_input(int16_t *s, int len) {
    if (len <= 0) return;
    if (!m_in && !m_cap_run) { memset(s, 0, sizeof(int16_t) * (size_t)len); return; }

    struct timespec t0, t1, ts;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    m_last_read = t0;
    m_have_read = true;
    int got = 0;
    while (got < len) {
        pthread_mutex_lock(&m_cap_mtx);
        while (m_cap_count > 0 && got < len) {
            s[got++] = m_cap_ring[m_cap_tail];
            m_cap_tail = (m_cap_tail + 1) % CAP_RING;
            m_cap_count--;
        }
        if (got < len && m_cap_run) {
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 20 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&m_cap_cv, &m_cap_mtx, &ts);
            pthread_mutex_unlock(&m_cap_mtx);
            // Keep the transmit loop pacing even if the capture side has gone
            // quiet: without this it would spin.
            clock_gettime(CLOCK_MONOTONIC, &t1);
            long waited = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
            if (waited > 200) break;
            continue;
        }
        bool running = m_cap_run;
        pthread_mutex_unlock(&m_cap_mtx);
        if (!running) break;
    }
    if (got < len) {
        m_starved += len - got;
        memset(s + got, 0, sizeof(int16_t) * (size_t)(len - got));
    }
#ifdef M17WEB_DIAG
    {
        static int ni = 0;
        static struct timespec tfirst;
        struct timespec tn;
        if (ni == 0) clock_gettime(CLOCK_MONOTONIC, &tfirst);
        if ((ni++ % 50) == 0) {
            clock_gettime(CLOCK_MONOTONIC, &tn);
            double secs = (tn.tv_sec - tfirst.tv_sec) + (tn.tv_nsec - tfirst.tv_nsec) / 1e9;
            long consumed = (long)ni * 160;
            fprintf(stdout, "diag-cap: in len=%d got=%d ring=%d starved=%ld consumed=%ld in %.2fs"
                            " = %.0f samples/s\n", len, got, m_cap_count, m_starved, consumed,
                    secs, secs > 0 ? consumed / secs : 0.0);
        }
        fflush(stdout);
    }
#endif
}

void audio_alsa_output(int16_t *s, int len) {
    if (!m_out) return;
    int done = 0;
    while (done < len) {
        snd_pcm_sframes_t n = snd_pcm_writei(m_out, s + done, (snd_pcm_uframes_t)(len - done));
        if (n > 0) { done += (int)n; continue; }
        if (n == -EPIPE) { snd_pcm_prepare(m_out); continue; }  // underrun
        if (n == -EAGAIN) { continue; }
        break;
    }
}

void audio_alsa_flush(void) {
    if (m_out) snd_pcm_drain(m_out);
    pthread_mutex_lock(&m_cap_mtx);
    // Throw away everything the card delivered before the transmitter was
    // ready: that is audio from before the over, and keeping it would make the
    // whole transmission lag behind the microphone by however long it took the
    // radio to start.  It is a deliberate flush, so it is not a drop.
    m_cap_head = m_cap_tail = m_cap_count = 0;
    m_starved = 0;
    pthread_mutex_unlock(&m_cap_mtx);
}
