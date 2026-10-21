// webui.cpp
//
// Browser front end for m17web.  Implements the presentation API (ui.h) on top
// of the small HTTP/WebSocket server in webserver.c:
//
//   * GET /            the single page UI (www/index.html or embedded assets)
//   * WS  /ws          JSON state / command channel plus binary voice audio
//
// Wire protocol (WebSocket)
//   server -> client   text   {"t":"state" | "log" | "resp" | "help" | "hello", ...}
//                      binary [0x02][s16le PCM] decoded receive audio
//   client -> server   text   an MMI command line ("rf 146400000"), or
//                              {"cmd":"rf 146400000"}
//                      binary [0x01][s16le PCM] microphone audio
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <signal.h>
#include <unistd.h>
#include "fargan.h"
#include "recorder.h"
#include <time.h>
#include <ctype.h>
#include <pthread.h>

#include "m17defines.h"
#include "bluetooth.h"
#include "webserver.h"
#include "audio.h"
#include "ui.h"

#define DEFAULT_WEB_PORT 8080
#define LOG_LINES        200
#define LOG_LEN          180
#define STATE_MIN_MS     100
#define AUDIO_TAG_MIC    0x01
#define AUDIO_TAG_SPK    0x02

#ifdef EMBED_WWW
#include "www_assets.h"
#else
static const WsAsset m_embedded_none[] = { { NULL, NULL, NULL, 0 } };
#endif

// forward declarations
static void help_send_all(void);
static void send_refs(void);

// ---------------------------------------------------------------------------
// UI state mirrored for the browser
// ---------------------------------------------------------------------------
static int   m_refs_count;                   // reflectors found in M17Hosts.txt
static int   m_port = DEFAULT_WEB_PORT;
static char  m_www_dir[512] = "www";
static void *m_srv;
static int   m_srv_ok;

static char  m_ptt[4]   = "RX";
static char  m_mode[16] = "gate";
static char  m_src[16]  = "        ";
static char  m_dst[16]  = "        ";
static double m_signal, m_power;
static bool  m_in_frame;
// Written by the signal handler, read by the command loop in another thread:
// must be volatile or the optimiser hoists the load out of the loop and the
// program never exits.
static volatile sig_atomic_t m_quit;

static char  m_log[LOG_LINES][LOG_LEN];
static int   m_log_count;      // total lines ever logged
static pthread_mutex_t m_log_mtx = PTHREAD_MUTEX_INITIALIZER;

static long  m_last_state_ms;
static long  m_frames_seen;

// ---------------------------------------------------------------------------
static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

static void json_escape(const char *in, char *out, int len) {
    int o = 0;
    for (const char *p = in; *p && o < len - 8; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else if (c < 0x20) { o += snprintf(&out[o], len - o, "\\u%04x", c); }
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

// ---------------------------------------------------------------------------
// Log ring buffer
// ---------------------------------------------------------------------------
static void log_push(const char *line) {
    pthread_mutex_lock(&m_log_mtx);
    int slot = m_log_count % LOG_LINES;
    snprintf(m_log[slot], LOG_LEN, "%s", line);
    m_log_count++;
    pthread_mutex_unlock(&m_log_mtx);
}

static void log_send(const char *kind, const char *fmt, ...) {
    char line[LOG_LEN], esc[LOG_LEN * 2], msg[LOG_LEN * 3];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    log_push(line);
    if (!m_srv) return;
    json_escape(line, esc, sizeof(esc));
    snprintf(msg, sizeof(msg), "{\"t\":\"log\",\"kind\":\"%s\",\"line\":\"%s\"}", kind, esc);
    ws_broadcast_text(m_srv, msg);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static const char *mode_name(CircuitType c) {
    switch (c) {
        case ASTOAS: return "loop";
        case ARTOAS: return "fm-radio";
        case DRTOAS: return "radio";
        case ASTODN: return "client";
        case ARTODN: return "fm-client";
        case DRTODN: return "gate";
        default:     return "?";
    }
}

static void decode_addr(uint48_t a, char *out, int len) {
    char tmp[16];
    m17_decode_call(a, tmp);
    int i = 0;
    // trim trailing spaces
    for (int k = 0; k < 9 && tmp[k] && i < len - 1; k++) out[i++] = tmp[k];
    while (i > 0 && out[i-1] == ' ') i--;
    out[i] = 0;
}

// Small append helper: building the state string as one 31 argument vararg
// call was both unreadable and fragile (one stray escape silently shifted every
// later argument).  Each append is checked by -Wformat on its own.
static int json_add(char *out, int len, int used, const char *fmt, ...) {
    if (used < 0) used = 0;
    if (used >= len - 1) return len - 1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + used, (size_t)(len - used), fmt, ap);
    va_end(ap);
    if (n < 0) return used;
    return (used + n < len) ? used + n : len - 1;
}

static void build_state(char *out, int len) {
    const M17_Dbase *db = m17_get_db();
    char mode[24];
    // Always the live channel type: the mode can be changed while running now,
    // so a saved string would go stale the moment it changed.
    snprintf(mode, sizeof(mode), "%s", mode_name(db->chan_type));
    for (int i = 0; mode[i]; i++)
        if (mode[i] == '\n' || mode[i] == '\r' || mode[i] == '\t') mode[i] = ' ';

    char ref[16] = "";
    m17_net_ref_address(ref);

    // The reflector module is only set once connected; %c of a NUL byte would
    // make the JSON unparseable.
    char mod = m17_net_ref_module();
    if (mod < 0x20 || mod > 0x7E) mod = '-';

    // Gateway callsign straight from the database (set from config.txt)
    char gate[16];
    snprintf(gate, sizeof(gate), "%s", db->tx_gate_add);
    for (int i = (int)strlen(gate) - 1; i >= 0 && (gate[i] == ' ' || gate[i] < 0x20); i--)
        gate[i] = 0;

    int o = 0;
    o = json_add(out, len, o, "{\"t\":\"state\",\"radio\":\"%s\"", radio_name());
    // The transmitter state as the radio sees it, so a key press from the
    // hardware switch or a reflector shows up in the interface too.
    const char *ptt_now = m_ptt;
    switch (m17_txrx_ptt_state()) {
        case 1:  ptt_now = "TX"; break;
        case 2:  ptt_now = "DX"; break;
        case 3:  ptt_now = "CA"; break;
        default: ptt_now = "RX"; break;
    }
    o = json_add(out, len, o, ",\"mode\":\"%s\",\"ptt\":\"%s\"", mode, ptt_now);
    {
        // "src" is what THIS station transmits with, and must not change when a
        // station is received: gui_save_src_address() is called by the receive
        // path, the reflector path and the transmit path alike, so the old
        // single variable showed the station being heard in the callsign box -
        // which is the box that sets what we transmit with.
        char my_src[16], rx_src[16], my_dst[16];
        if (db->tx_src_add[0]) {
            snprintf(my_src, sizeof(my_src), "%.9s", db->tx_src_add);
            for (char *q = my_src; *q; q++) *q = (char)toupper((unsigned char)*q);
        } else {
            decode_addr(db->tx_src, my_src, sizeof(my_src));
        }
        for (char *q = my_src; *q; q++) if (q[0] == ' ' && q[1] == 0) { *q = 0; break; }
        if (db->rx_src_add[0]) {
            snprintf(rx_src, sizeof(rx_src), "%.9s", db->rx_src_add);
            for (char *q = rx_src; *q; q++) *q = (char)toupper((unsigned char)*q);
        } else {
            decode_addr(db->rx_src, rx_src, sizeof(rx_src));
        }
        for (char *q = rx_src; *q; q++) if (q[0] == ' ' && q[1] == 0) { *q = 0; break; }
        // The destination display is set by the interface; when it has nothing
        // to say, report what the database actually holds.
        snprintf(my_dst, sizeof(my_dst), "%s", (m_dst[0] && m_dst[0] != ' ') ? m_dst : "");
        if (!my_dst[0]) decode_addr(db->tx_dest, my_dst, sizeof(my_dst));
        o = json_add(out, len, o, ",\"src\":\"%s\",\"rx_src\":\"%s\",\"dst\":\"%s\",\"gate\":\"%s\"",
                     my_src, rx_src, my_dst, gate);
    }
    o = json_add(out, len, o, ",\"rx_freq\":%llu,\"tx_freq\":%llu",
                 (unsigned long long)db->rx_freq, (unsigned long long)db->tx_freq);
    o = json_add(out, len, o, ",\"rx_gain\":%.3f,\"tx_gain\":%.3f", radio_get_rx_gain(), radio_get_tx_gain());
    o = json_add(out, len, o, ",\"fc\":%.6f", radio_get_freq_correction());
    o = json_add(out, len, o, ",\"afc\":%s,\"listen_all\":%s,\"in_frame\":%s",
                 radio_get_afc_status() ? "true" : "false",
                 db->listen_flag ? "true" : "false",
                 (m_in_frame || db->in_frame) ? "true" : "false");
    o = json_add(out, len, o, ",\"signal\":%.3f,\"power\":%.3f", m_signal, m_power);
    o = json_add(out, len, o, ",\"frames\":%u,\"gerr\":%u", db->n_frames, db->g_errors);
    o = json_add(out, len, o, ",\"ref\":{\"name\":\"%s\",\"module\":\"%c\",\"connected\":%s}",
                 ref, mod, m17_net_connected() ? "true" : "false");
    {
        // The two directions are routed separately, which is what the Audio
        // configuration box selects; "audio" keeps the old whole-pair summary
        // for anything that still reads it (a top bar, a script).
        char sum[48];
        audio_route_summary(sum, sizeof(sum));
        o = json_add(out, len, o, ",\"audio\":\"%s\",\"audio_pending\":%s"
                     ",\"spk_route\":\"%s\",\"mic_route\":\"%s\""
                     ",\"mic_gain\":%.3f,\"spk_vol\":%.3f",
                     sum,
                     audio_backend_pending() ? "true" : "false",
                     audio_route_name(audio_get_route(true)),
                     audio_route_name(audio_get_route(false)),
                     audio_get_mic_gain(), audio_get_spk_vol());
    }
    o = json_add(out, len, o, ",\"pulse\":{\"compiled\":%s,\"ready\":%s,\"error\":\"%s\"}",
                 audio_have_pulse() ? "true" : "false",
                 (audio_get_backend() == AUDIO_PULSE && audio_local_ready()) ? "true" : "false",
                 (audio_get_backend() == AUDIO_PULSE) ? audio_backend_error() : "");
    o = json_add(out, len, o, ",\"alsa\":{\"compiled\":%s,\"ready\":%s,\"device\":\"%s\",\"error\":\"%s\"}",
                 audio_have_alsa() ? "true" : "false",
                 (audio_get_backend() == AUDIO_ALSA && audio_local_ready()) ? "true" : "false",
                 audio_local_device(),
                 (audio_get_backend() == AUDIO_ALSA) ? audio_backend_error() : "");
    // The neural receiver: receive-only, so this says what this station does with
    // what it hears, not anything about what other stations will hear.
    o = json_add(out, len, o, ",\"fargan\":{\"compiled\":%s,\"on\":%s}",
                 fargan_rx_compiled() ? "true" : "false",
                 fargan_rx_enabled() ? "true" : "false");
    {
        // The synthesis thread's health: frames queued and decoded, how many
        // were dropped because it could not keep up, and how many times the
        // audio path found nothing ready.  All three stay at zero when it is
        // keeping pace, which is the whole question on a Pi.
        unsigned long fq = 0, fd = 0, fdrop = 0, fdry = 0;
        fargan_rx_stats(&fq, &fd, &fdrop, &fdry);
        o = json_add(out, len, o, ",\"fargan_rx\":{\"queued\":%lu,\"decoded\":%lu,"
                     "\"dropped\":%lu,\"dry\":%lu}", fq, fd, fdrop, fdry);
    }
    {
        // The recorder: whether it is running, which file, and how long.  Not a
        // remembered setting, so this says what is actually happening.
        unsigned long rs = 0;
        unsigned int  rsec = 0;
        rec_stats(&rs, &rsec);
        o = json_add(out, len, o, ",\"record\":{\"on\":%s,\"file\":\"%s\",\"rate\":%d,"
                     "\"seconds\":%u,\"samples\":%lu,\"error\":\"%s\"}",
                     rec_active() ? "true" : "false", rec_file(), rec_rate(), rsec, rs, rec_error());
    }
    // What Bluetooth knows, from the cache the "bt" command refreshes - this
    // runs on the radio's thread and must never shell out.  Output and input are
    // reported separately: they are chosen separately, and one may be the sound
    // card while the other is Bluetooth.
    o = json_add(out, len, o, ",\"bt\":{\"available\":%s,\"why\":\"%s\",\"adapter\":\"%s\",\"powered\":%s,"
                 "\"out\":{\"mac\":\"%s\",\"name\":\"%s\",\"profile\":\"%s\",\"connected\":%s},"
                 "\"in\":{\"mac\":\"%s\",\"name\":\"%s\",\"connected\":%s},\"devices\":[",
                 bt_available() ? "true" : "false", bt_available_why(), bt_adapter(),
                 bt_powered() ? "true" : "false",
                 bt_out_mac(), bt_out_name(), bt_out_profile(),
                 bt_out_connected() ? "true" : "false",
                 bt_in_mac(), bt_in_name(), bt_in_connected() ? "true" : "false");
    for(int i = 0; i < bt_count(); i++)
        o = json_add(out, len, o, "%s{\"mac\":\"%s\",\"name\":\"%s\",\"icon\":\"%s\",\"connected\":%s,\"paired\":%s,\"out\":%s,\"in\":%s,"
                 "\"a2dp\":%s,\"hfp\":%s}",
                     i ? "," : "", bt_mac(i), bt_name(i), bt_icon(i),
                     bt_connected(i) ? "true" : "false",
                     bt_paired(i) ? "true" : "false",
                     strcmp(bt_out_mac(), bt_mac(i)) == 0 ? "true" : "false",
                     strcmp(bt_in_mac(),  bt_mac(i)) == 0 ? "true" : "false",
                     bt_device_a2dp(bt_mac(i)) ? "true" : "false",
                     bt_device_hfp(bt_mac(i)) ? "true" : "false");
    o = json_add(out, len, o, "]}");
    o = json_add(out, len, o, ",\"mic_level\":%.3f,\"spk_level\":%.3f",
                 audio_get_mic_level(), audio_get_spk_level());
    // Playback underruns: the number that separates "the card ran dry" from
    // "nothing arrived to play" when a report of dropouts comes in.
    o = json_add(out, len, o, ",\"spk_xruns\":%ld", audio_alsa_spk_xruns());
    o = json_add(out, len, o, ",\"tx_lead\":%d,\"tx_drops\":%ld,\"tx_overruns\":%ld",
                 m17_tx_lead_frames(), audio_tx_dropped(), audio_tx_overruns());
    {
        bool mic = false, spk = false;
        audio_local_streams(&mic, &spk);
        o = json_add(out, len, o, ",\"mic_open\":%s,\"spk_open\":%s",
                     mic ? "true" : "false", spk ? "true" : "false");
        // The card's own controls, so the mixer panel can show what is really
        // set rather than what was last asked for.  Names come from the card;
        // anything outside a safe set is replaced, so the JSON cannot break.
        o = json_add(out, len, o, ",\"mixer\":[");
        int nn = audio_alsa_mixer_count();
        for (int i = 0; i < nn; i++) {
            char safe[32];
            size_t k = 0;
            for (const char *p = audio_alsa_mixer_name(i); *p && k < sizeof(safe) - 1; p++) {
                char ch = *p;
                bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                          (ch >= '0' && ch <= '9') || ch == ' ' || ch == '-' || ch == '_';
                safe[k++] = ok ? ch : '_';
            }
            safe[k] = 0;
            o = json_add(out, len, o, "%s{\"name\":\"%s\",\"pct\":%d,\"sw\":%s,\"on\":%s,\"mic\":%s}",
                         i ? "," : "", safe, audio_alsa_mixer_pct(i),
                         audio_alsa_mixer_has_switch(i) ? "true" : "false",
                         audio_alsa_mixer_on(i) ? "true" : "false",
                         audio_alsa_mixer_is_capture(i) ? "true" : "false");
        }
        o = json_add(out, len, o, "]");
    }
    {
        // Position: the fix, its age, and the Maidenhead locator for the UI
        char grid[12], dev[160];
        bool fresh = gps_fix_fresh(60000);
        gps_maidenhead(grid, sizeof(grid));
        snprintf(dev, sizeof(dev), "%s", gps_status());
        for (char *q = dev; *q; q++) if (*q == '"' || *q == '\\') *q = '-';
        {
            // Position heard from another station in its LSF META field
            GpsMsg rx; char rsrc[16] = "";
            if (gps_rx_position(&rx, rsrc, sizeof(rsrc))) {
                char rgrid[12];
                gps_maidenhead_of(rx.lat, rx.lon, rgrid, sizeof(rgrid));
                o = json_add(out, len, o, ",\"heard\":{\"src\":\"%s\",\"lat\":%.6f,\"lon\":%.6f,"
                             "\"grid\":\"%s\",\"alt_m\":%d,\"speed_kmh\":%.1f,\"course\":%d,\"age_s\":%.1f",
                             rsrc, rx.lat, rx.lon, rgrid, (int)rx.alt, rx.speed_kmh, (int)rx.course,
                             gps_rx_age_ms() / 1000.0);
                // Bearing and distance only mean something with a fix of our own
                if (gps_fix_fresh(60000)) {
                    o = json_add(out, len, o, ",\"bearing\":%.1f,\"distance_km\":%.2f",
                                 gps_bearing(gps_lat(), gps_lon(), rx.lat, rx.lon),
                                 gps_distance_km(gps_lat(), gps_lon(), rx.lat, rx.lon));
                }
                o = json_add(out, len, o, "}");
            }
        }
        o = json_add(out, len, o, ",\"gps\":{\"valid\":%s,\"fresh\":%s,\"tx\":%s,\"meta\":%s,\"lat\":%.6f,\"lon\":%.6f,"
                     "\"grid\":\"%s\",\"alt_m\":%d,\"speed_kmh\":%.1f,\"course\":%d,\"hdop\":%.2f,"
                     "\"sats\":%d,\"age_s\":%.1f,\"status\":\"%s\","
                     "\"csv\":{\"on\":%s,\"path\":\"%s\",\"rows\":%d,\"interval\":%d}}",
                     m_gps.valid ? "true" : "false", fresh ? "true" : "false",
                     m17_tx_gnss() ? "true" : "false",
                     m17_tx_gnss_enabled() ? "true" : "false",
                     m_gps.lat, m_gps.lon, grid, (int)m_gps.alt, m_gps.speed_kmh, (int)m_gps.course,
                     m_gps.hdop, (int)m_gps.nsats, gps_fix_age_ms() / 1000.0, dev,
                     gps_csv_on() ? "true" : "false", gps_csv_path(),
                     gps_csv_rows(), gps_csv_interval());
    }
    {
        // Hardware PTT on the Pi's header (see rpi_gpio.cpp)
        char gerr[200];
        snprintf(gerr, sizeof(gerr), "%s", rpi_gpio_error());
        for (char *q = gerr; *q; q++) if (*q == '"' || *q == '\\') *q = '-';
        o = json_add(out, len, o, ",\"gpio\":{\"built\":%s,\"ok\":%s,\"pressed\":%s,\"hw_tx\":%s,"
                     "\"out\":%d,\"in\":%d,\"error\":\"%s\"}",
#ifdef HAVE_GPIOD
                     "true",
#else
                     "false",
#endif
                     rpi_gpio_ok() ? "true" : "false",
                     rpi_gpio_pressed() ? "true" : "false",
                     m17_tx_hw_ptt() ? "true" : "false",
                     rpi_gpio_out_pin(), rpi_gpio_in_pin(), gerr);
    }
    o = json_add(out, len, o, ",\"clients\":%d", m_srv ? ws_client_count(m_srv) : 0);
    o = json_add(out, len, o, ",\"refs\":%d", m_refs_count);
    o = json_add(out, len, o, ",\"rx_stream\":%lu", (unsigned long)(db->n_frames - m_frames_seen));
    o = json_add(out, len, o, ",\"uptime\":%ld}", (long)(now_ms() / 1000));
    out[o] = 0;
}

static void push_state(void) {
    if (!m_srv || ws_client_count(m_srv) == 0) return;
    // Throttle: the RX path calls us up to 25 times a second, which is far more
    // than a browser needs.  The tick callback guarantees a refresh anyway.
    long now = now_ms();
    if (m_last_state_ms != 0 && (now - m_last_state_ms) < STATE_MIN_MS) return;
    char st[4096];
    build_state(st, sizeof(st));
    // json_add truncates rather than overrunning, so a state that outgrew its
    // buffer would go out as broken JSON and every browser would ignore it
    // (the page then sits frozen, showing nothing).  Never send that: say so in
    // the log, and send a valid message carrying no fields.
    size_t sl = strlen(st);
    if (sl == 0 || st[sl - 1] != '}') {
        printf("m17web: state message outgrew its buffer (%d bytes) - report this\n", (int)sizeof(st));
        fflush(stdout);
        ws_broadcast_text(m_srv, "{\"t\":\"state\"}");
    } else {
        ws_broadcast_text(m_srv, st);
    }
    const M17_Dbase *db = m17_get_db();
    m_frames_seen = db->n_frames;
    m_last_state_ms = now;
}

// ---------------------------------------------------------------------------
// WebSocket callbacks (run on the web server thread)
// ---------------------------------------------------------------------------
static void send_hello(int id) {
    if (!m_srv) return;
#ifndef M17WEB_STAMP
#define M17WEB_STAMP "unknown"
#endif
    char hello[256];
    snprintf(hello, sizeof(hello),
             "{\"t\":\"hello\",\"app\":\"m17web\",\"proto\":1,\"stamp\":\"%s\","
             "\"audio_tag_mic\":1,\"audio_tag_spk\":2,\"rate\":8000}", M17WEB_STAMP);
    ws_send_text(m_srv, id, hello);

    // Help text
    char msg[4096];
    int o = snprintf(msg, sizeof(msg), "{\"t\":\"help\",\"lines\":[");
    const char *const *h = ui_help_lines();
    for (int i = 0; h[i] && o < (int)sizeof(msg) - 128; i++) {
        char esc[LOG_LEN * 2];
        json_escape(h[i], esc, sizeof(esc));
        o += snprintf(&msg[o], sizeof(msg) - o, "%s\"%s\"", i ? "," : "", esc);
    }
    snprintf(&msg[o], sizeof(msg) - o, "]}");
    ws_send_text(m_srv, id, msg);

    // Log backlog
    pthread_mutex_lock(&m_log_mtx);
    int n = m_log_count < LOG_LINES ? m_log_count : LOG_LINES;
    int start = m_log_count - n;
    for (int i = 0; i < n; i++) {
        char esc[LOG_LEN * 2], msg2[LOG_LEN * 3];
        json_escape(m_log[(start + i) % LOG_LINES], esc, sizeof(esc));
        snprintf(msg2, sizeof(msg2), "{\"t\":\"log\",\"kind\":\"old\",\"line\":\"%s\"}", esc);
        ws_send_text(m_srv, id, msg2);
    }
    pthread_mutex_unlock(&m_log_mtx);

    char st[4096];
    build_state(st, sizeof(st));
    size_t sl = strlen(st);
    if (sl == 0 || st[sl - 1] != '}') {
        printf("m17web: state message outgrew its buffer (%d bytes) - report this\n", (int)sizeof(st));
        fflush(stdout);
        ws_send_text(m_srv, id, "{\"t\":\"state\"}");
    } else {
        ws_send_text(m_srv, id, st);
    }
}

static void web_on_open(const WsClientInfo *ci, void *user) {
    (void)user;
    log_send("info", "client connected: %s", ci->peer);
    // Read the reflector list first: the greeting pushes the state, and that
    // should already report how many reflectors were found.  Waiting for the page
    // to ask for the list left the drop down empty whenever the answer arrived
    // before the page was listening for it.
    send_refs();
    send_hello(ci->id);
}

static void web_on_close(int id, void *user) {
    (void)user;
    log_send("info", "client %d disconnected", id);
}

// Pull "cmd":"..." out of a JSON object without a full parser
static int extract_cmd(const char *in, char *out, int len) {
    const char *p = strstr(in, "\"cmd\"");
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ') p++;
    if (*p != '"') return -1;
    p++;
    int o = 0;
    while (*p && *p != '"' && o < len - 1) {
        if (*p == '\\' && p[1]) p++;
        out[o++] = *p++;
    }
    out[o] = 0;
    return 0;
}

static void run_command(const char *cmd_in) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s", cmd_in);

    // strip whitespace
    int n = (int)strlen(cmd);
    while (n > 0 && isspace((unsigned char)cmd[n-1])) cmd[--n] = 0;
    char *s = cmd;
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return;

    // Web-only conveniences
    if (strcasecmp(s, "q") == 0 || strcasecmp(s, "quit") == 0) {
        log_send("info", "quit requested by browser");
        m17_txrx_disable();
        gui_update();
        m_quit = true;
        return;
    }
    if (strcasecmp(s, "refs") == 0) {
        send_refs();
        return;
    }
    // Show what was just asked for: m_dst otherwise only updates when a
    // transmission goes out, so a destination set for a test stayed invisible.
    if (strncasecmp(s, "da ", 3) == 0) {
        const char *v = s + 3;
        while (*v == ' ') v++;
        if (*v == '#') v++;                       // '#' is not an M17 character
        snprintf(m_dst, sizeof(m_dst), "%.9s", v);
        for (char *q = m_dst; *q; q++) *q = (char)toupper((unsigned char)*q);
    } else if (strcasecmp(s, "ba") == 0 || strcasecmp(s, "broadcast") == 0) {
        snprintf(m_dst, sizeof(m_dst), "BROADCAST");
    }
    if (strcasecmp(s, "b") == 0 || strcasecmp(s, "break") == 0) {
        m17_txrx_ptt_off();
        gui_cmd_resp("OK");
        gui_update();
        return;
    }
    if (strcasecmp(s, "clients") == 0) {
        log_send("info", "%d client(s) connected", m_srv ? ws_client_count(m_srv) : 0);
        gui_cmd_resp("OK");
        return;
    }

    mmi_parse(cmd);
    gui_update();
}

//
// The reflector list comes from M17Hosts.txt (designator + address), the same
// file m17_net.cpp uses to resolve a reflector by name.
//
static void send_refs(void) {
    if (!m_srv) return;
    char msg[8192];
    int count = 0;
    int o = snprintf(msg, sizeof(msg), "{\"t\":\"refs\",\"names\":[");
    FILE *fp = fopen("M17Hosts.txt", "r");
    int first = 1;
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '#' || *p == '\n' || *p == 0) continue;
            char name[32];
            if (sscanf(p, "%31s", name) != 1) continue;
            if (o > (int)sizeof(msg) - 64) break;
            // Operators know reflectors by their full name ("M17-M17", as in the
            // published list); older copies of M17Hosts.txt hold the bare
            // designator.  Show them all as "M17-<designator>"; m17_net.cpp
            // matches either form.
            const char *bare = (strncasecmp(name, "M17-", 4) == 0) ? "" : "M17-";
            o += snprintf(&msg[o], sizeof(msg) - o, "%s\"%s%s\"", first ? "" : ",", bare, name);
            first = 0;
            m_refs_count = ++count;
        }
        fclose(fp);
    }
    snprintf(&msg[o], sizeof(msg) - o, "]}");
    ws_broadcast_text(m_srv, msg);
}

static void web_on_msg(int id, int is_binary, const uint8_t *data, int len, void *user) {
    (void)id; (void)user;
    if (is_binary) {
        if (len > 1 && data[0] == AUDIO_TAG_MIC) {
            int n = (len - 1) / 2;
            audio_web_mic_push((const int16_t *)(data + 1), n);
        }
        return;
    }
    char cmd[256];
    const char *txt = (const char *)data;
    if (len > 0 && txt[0] == '{') {
        if (extract_cmd(txt, cmd, sizeof(cmd)) == 0) run_command(cmd);
        return;
    }
    int n = len < (int)sizeof(cmd) - 1 ? len : (int)sizeof(cmd) - 1;
    memcpy(cmd, data, n);
    cmd[n] = 0;
    run_command(cmd);
}

static void web_on_tick(void *user) {
    (void)user;
    if (!m_srv) return;
    if (m_last_state_ms == 0 || (now_ms() - m_last_state_ms) >= 250) push_state();
}

// ---------------------------------------------------------------------------
// Audio sink used by the browser back end
// ---------------------------------------------------------------------------
void webui_audio_to_browser(const int16_t *s, int len) {
    if (!m_srv || len <= 0) return;
    if (ws_client_count(m_srv) == 0) return;
    static uint8_t buf[1 + 2048];
    int maxsamples = (int)(sizeof(buf) - 1) / 2;
    int off = 0;
    while (off < len) {
        int chunk = len - off;
        if (chunk > maxsamples) chunk = maxsamples;
        buf[0] = AUDIO_TAG_SPK;
        memcpy(buf + 1, s + off, (size_t)chunk * 2);
        ws_broadcast_bin(m_srv, buf, 1 + chunk * 2);
        off += chunk;
    }
}

// ---------------------------------------------------------------------------
// Presentation API
// ---------------------------------------------------------------------------
void ui_set_web_port(int port) { m_port = port; }
int  ui_get_web_port(void) { return m_port; }
void ui_set_www_dir(const char *dir) {
    if (dir && dir[0]) snprintf(m_www_dir, sizeof(m_www_dir), "%s", dir);
}
const char *ui_get_www_dir(void) { return m_www_dir; }

UiBackend ui_backend(void) { return UI_BACKEND_WEB; }

void gui_open(void) {
    WsConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = m_port;
    cfg.www_dir = m_www_dir;
#ifdef EMBED_WWW
    cfg.assets = www_assets;
#else
    cfg.assets = m_embedded_none;
#endif
    cfg.on_open  = web_on_open;
    cfg.on_close = web_on_close;
    cfg.on_msg   = web_on_msg;
    cfg.on_tick  = web_on_tick;
    cfg.tick_ms  = 200;
    m_srv = ws_start(&cfg);
    if (m_srv) {
        m_srv_ok = 1;
        printf("m17web: web UI on http://0.0.0.0:%d/  (assets: %s%s)\n",
               m_port, m_www_dir, access(m_www_dir, R_OK) == 0 ? "" : " [not found, using embedded assets]");
        log_send("info", "m17web started, web UI on port %d", m_port);
    } else {
        fprintf(stderr, "m17web: failed to start the web server on port %d\n", m_port);
        log_send("err", "web server failed to start on port %d", m_port);
    }
}

void gui_close(void) {
    if (m_srv) {
        ws_stop(m_srv);
        m_srv = NULL;
    }
    m_srv_ok = 0;
}

void gui_update(void) { push_state(); }

void gui_update_clear(void) {
    pthread_mutex_lock(&m_log_mtx);
    m_log_count = 0;
    pthread_mutex_unlock(&m_log_mtx);
    if (m_srv) ws_broadcast_text(m_srv, "{\"t\":\"clear\"}");
}

void gui_los(void) {
    m_in_frame = false;
    log_send("rx", "loss of signal");
    push_state();
}

void gui_aos(void) {
    m_in_frame = true;
    push_state();
}

void gui_cmd_prompt(void) { }

void gui_cmd_resp(const char *resp) {
    char line[LOG_LEN];
    snprintf(line, sizeof(line), "%s", resp ? resp : "");
    int n = (int)strlen(line);
    while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r' || line[n-1] == ' ')) line[--n] = 0;
    log_push(line);
    if (m_srv) {
        char esc[LOG_LEN * 2], msg[LOG_LEN * 3];
        json_escape(line, esc, sizeof(esc));
        snprintf(msg, sizeof(msg), "{\"t\":\"resp\",\"line\":\"%s\"}", esc);
        ws_broadcast_text(m_srv, msg);
    } else {
        printf("%s\n", line);
    }
}

void gui_help(void) { help_send_all(); }

// (kept separate so gui_help and the connect message share one implementation)
static void help_send_all(void) {
    if (!m_srv) return;
    char msg[4096];
    int o = snprintf(msg, sizeof(msg), "{\"t\":\"help\",\"lines\":[");
    const char *const *h = ui_help_lines();
    for (int i = 0; h[i] && o < (int)sizeof(msg) - 128; i++) {
        char esc[LOG_LEN * 2];
        json_escape(h[i], esc, sizeof(esc));
        o += snprintf(&msg[o], sizeof(msg) - o, "%s\"%s\"", i ? "," : "", esc);
    }
    snprintf(&msg[o], sizeof(msg) - o, "]}");
    ws_broadcast_text(m_srv, msg);
}

void gui_bar(double v) { gui_signal(v); }

void gui_signal(double v) {
    m_signal = v;
    push_state();
}

void gui_power(double v) {
    m_power = v;
}

void gui_char(char c) { (void)c; }

void gui_save_src_address(uint48_t add) {
    decode_addr(add, m_src, sizeof(m_src));
    log_send("rx", "src %s", m_src);
    push_state();
}

void gui_save_dest_address(uint48_t add) {
    decode_addr(add, m_dst, sizeof(m_dst));
    push_state();
}

void gui_save_mode(const char *mode) {
    snprintf(m_mode, sizeof(m_mode), "%s", mode ? mode : "");
    // keep the JSON clean whatever the caller hands us
    for (int i = 0; m_mode[i]; i++)
        if (m_mode[i] == '\n' || m_mode[i] == '\r' || m_mode[i] == '\t') m_mode[i] = ' ';
    push_state();
}

void gui_tx(void) {
    snprintf(m_ptt, sizeof(m_ptt), "TX");
    push_state();
}

void gui_rx(void) {
    snprintf(m_ptt, sizeof(m_ptt), "RX");
    push_state();
}

void gui_dp(void) {
    snprintf(m_ptt, sizeof(m_ptt), "DX");
    push_state();
}

// ---------------------------------------------------------------------------
// Command/quit loop
// ---------------------------------------------------------------------------
static void on_signal(int sig) {
    (void)sig;
    m_quit = true;
    m17_txrx_disable();
}

void *ui_command_loop(void *arg) {
    (void)arg;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    log_send("info", "web UI ready: http://<this host>:%d/", m_port);
    while (!m_quit) {
        usleep(100 * 1000);
        if (access(m_www_dir, R_OK) != 0) {
            // keep serving embedded assets; nothing to do
        }
    }
    // Give the browser a chance to see the shutdown message
    log_send("info", "shutting down");
    usleep(150 * 1000);
    return arg;
}
