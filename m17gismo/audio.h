// audio.h
//
// Audio routing for m17web.
//
// The M17 voice path always works with 8 kHz mono signed 16 bit PCM in blocks
// of codec2_samples_per_frame() samples (160 for CODEC2_MODE_3200).
//
// Where that audio comes from and goes to is selectable at run time:
//
//   pulse    local sound card via PulseAudio simple API
//   browser  microphone / speaker in the web browser, carried over WebSocket
//   file     raw 8 kHz s16le files (headless testing, no sound hardware)
//   both     browser in/out plus PulseAudio out
//   none     black hole (silence in, discard out)
//
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    AUDIO_NONE = 0,
    AUDIO_ALSA,      // the Pi's sound card, directly through ALSA
    AUDIO_PULSE,     // the Pi's sound card, through PulseAudio
    AUDIO_BROWSER,   // microphone/speaker in the web page
    AUDIO_FILE,
    AUDIO_BOTH
} AudioBackend;

// Selects the routing (call before audio_mic_open/audio_spk_open)
bool           audio_set_backend(AudioBackend b);
AudioBackend   audio_parse_backend(const char *name);
const char    *audio_backend_name(AudioBackend b);
AudioBackend   audio_get_backend(void);
void           audio_backend_status(char *out, int len);   // human readable

// Public API used by the M17 code (implemented by audio.cpp, the dispatcher)
void audio_mic_open(void);
void audio_mic_close(void);
void audio_spk_open(void);
void audio_spk_close(void);
void audio_input(int16_t *s, int len);          // blocks until len samples or timeout
void audio_pace_reset(void);                    // restart real-time pacing (key-down)
void audio_output(int16_t *s, int len);
void audio_input_flush(void);
void audio_output_flush(void);
double audio_get_mic_level(void);               // 0..1 RMS for the UI meter
double audio_get_spk_level(void);

// Is a backend change waiting to be applied (takes effect when the audio paths
// are next opened, i.e. at the next transmit/receive cycle)?
bool           audio_backend_pending(void);
// The device behind the current back end changed (a Bluetooth speaker was
// chosen, say).  Nothing else needs touching: this makes the M17 code close
// and open its streams again at the next cycle, which is how the change takes
// effect without a restart.
void           audio_request_reopen(void);
// Last error from the active backend, "" if none. e.g. PulseAudio not running.
const char    *audio_backend_error(void);
// Which back ends were compiled in, and whether the local sound card streams
// are actually open (false when PULSE was not compiled in)
bool           audio_have_pulse(void);
bool           audio_have_alsa(void);
bool           audio_local_ready(void);
const char    *audio_local_device(void);   // e.g. plughw:3,0 for the ALSA path
bool           audio_have_browser_audio(void);
// Transmit-health counters for the active backend: samples of microphone audio
// lost (card overrun, or padded with silence because the source starved) and
// the number of card overruns behind them.  These make transmit dropouts
// visible instead of silent.
void           audio_local_streams(bool *mic, bool *spk);   // what is open right now
long           audio_tx_dropped(void);
long           audio_tx_overruns(void);

// ---- ALSA back end (audio_alsa.cpp, needs -lasound) ----------------------
void audio_alsa_mic_open(void);
void audio_alsa_mic_close(void);
void audio_alsa_spk_open(void);
void audio_alsa_spk_close(void);
void audio_alsa_input(int16_t *s, int len);
void audio_alsa_output(int16_t *s, int len);
void audio_alsa_flush(void);
bool audio_alsa_ready(void);
long audio_alsa_overruns(void);
long audio_alsa_starved(void);
bool audio_alsa_mic_is_open(void);
bool audio_alsa_spk_is_open(void);
const char *audio_alsa_error(void);
const char *audio_alsa_device(void);
void audio_alsa_set_device(const char *dev);
// Capture can be chosen separately: a Bluetooth speaker is playback only, so
// the microphone stays on the sound card, while a Bluetooth headset uses one
// device for both directions.  Empty means "the same as playback".
void audio_alsa_set_capture_device(const char *dev);
const char *audio_alsa_capture_device(void);
// Capture on the card while playback goes somewhere else (a Bluetooth
// speaker), so the microphone does not follow the output.
void audio_alsa_capture_follow_card(void);
// Are the streams open on something other than what is configured now?  Used
// to tell a real device change from re-selecting what is already in use.
bool audio_alsa_device_changed(void);
// True when a device was asked for by name (a Bluetooth PCM, or a card), as
// opposed to "whatever card is present".  A named device is retried slowly
// forever: it may be switched on long after the station started.
bool audio_alsa_device_requested(void);
// Did the last open have to fall back to the sound card?  A Bluetooth device
// that is still coming up needs another attempt, not a silent station.
bool audio_alsa_fell_back(void);
// The card's own hardware mixer controls.  Names come from the card, so this
// works for whatever is plugged in; values are percentages of what the control
// allows, which gives the UI one slider shape for every card.  Setting one
// remembers it, and it is re-applied whenever the audio is opened.
int  audio_alsa_mixer_count(void);
const char *audio_alsa_mixer_name(int i);
int  audio_alsa_mixer_pct(int i);
bool audio_alsa_mixer_has_switch(int i);
bool audio_alsa_mixer_on(int i);
bool audio_alsa_mixer_is_capture(int i);
bool audio_alsa_mixer_set(const char *name, int pct);
void audio_alsa_mixer_apply(void);

// ---- PulseAudio back end (audio_pulse.cpp, needs -lpulse-simple) -----------
void audio_pulse_mic_open(void);
void audio_pulse_mic_close(void);
void audio_pulse_spk_open(void);
void audio_pulse_spk_close(void);
void audio_pulse_input(int16_t *s, int len);
void audio_pulse_output(int16_t *s, int len);
void audio_pulse_flush(void);
bool audio_pulse_ready(void);
const char *audio_pulse_error(void);

// ---- Browser back end (audio_web.cpp) -------------------------------------
void audio_web_mic_open(void);
void audio_web_mic_close(void);
void audio_web_spk_open(void);
void audio_web_spk_close(void);
void audio_web_mic_push(const int16_t *s, int len);   // from the WebSocket client
int  audio_web_mic_available(void);
int  audio_web_input(int16_t *s, int len);            // returns samples delivered
long audio_web_mic_starved(void);                     // samples padded with silence
void audio_web_mic_flush(void);                       // drop pre-transmission audio
void audio_web_output(int16_t *s, int len);           // forwarded to the browser
bool audio_web_active(void);

// WebSocket sink used by the browser back end, implemented in webui.cpp
void webui_audio_to_browser(const int16_t *s, int len);

// ---- File back end (audio_file.cpp) --------------------------------------
void audio_file_set(const char *in_path, const char *out_path);
void audio_file_mic_open(void);
void audio_file_mic_close(void);
void audio_file_spk_open(void);
void audio_file_spk_close(void);
void audio_file_input(int16_t *s, int len);
void audio_file_output(int16_t *s, int len);
bool audio_file_eof(void);
long audio_file_frames_read(void);
long audio_file_frames_written(void);

#endif // AUDIO_H
