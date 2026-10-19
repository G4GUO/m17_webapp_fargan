// bluetooth.cpp
//
// Bluetooth audio: choosing a speaker and a microphone and putting them into
// the audio path.  See bluetooth.h for what this is and why it is optional.
//
// The work is all in the ALSA device strings - BlueALSA has already turned the
// Bluetooth device into a PCM that the ALSA back end can open.  Everything that
// touches the radio or the Bluetooth daemon goes through bluetoothctl, which is
// why every MAC address is validated before it reaches a command line.
//
// Output and input are chosen *separately*.  A2DP is playback only, so a speaker
// used for output leaves the microphone on the sound card (or on another
// device); HFP carries both directions, so a headset can be the output, the
// input, or both.  A single device with a single "role" could not express
// "SoundCore for sound, BH-M10B for the microphone", which is a normal thing to
// want - and it is why the two selections are kept apart here.
//
// Two further things are not obvious.  A device that has been *discovered* is
// not a device that can be *connected*: BlueZ will not connect an unpaired
// device on behalf of a one-shot command, and bluetoothctl registers no pairing
// agent unless it is asked to - so the connect path pairs first (with a
// NoInputNoOutput agent, which is what a "Just Works" speaker needs) when
// "Paired: no".  And the connected flag cannot be read from the Bluetooth daemon
// on every state message: the state goes out several times a second and every
// bluetoothctl call is a D-Bus round trip.  A small monitor thread polls the
// cheap question and rebuilds the whole list now and then, so the UI cannot sit
// on a stale "connected".
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>

#include "bluetooth.h"
#include "audio.h"      // audio_request_reopen(): a device that comes up later is picked up

typedef struct {
    char mac[BT_MAC_LEN];
    char name[BT_NAME_LEN];
    char icon[BT_ICON_LEN];
    bool conn;
    bool paired;
} bt_dev_t;

static bool m_avail_checked;
static bool m_avail;
static char m_why[96];

static char m_adapter[BT_MAC_LEN];
static bool m_powered;

static bt_dev_t m_dev[BT_MAX_DEV];
static int      m_count;

// The two selections.  An empty mac means "the sound card" for that direction.
static char m_out_mac[BT_MAC_LEN];
static char m_out_prof[6];                 // "a2dp" | "sco"
static char m_in_mac[BT_MAC_LEN];

// Serialises the bluetoothctl calls.  The monitor thread and the command thread
// both make them, and two concurrent pairs or connects fight each other.  The
// state readers deliberately do not take it: they read the published snapshot,
// which bt_refresh() swaps in one step so the device list never flickers.
static pthread_mutex_t m_lock = PTHREAD_MUTEX_INITIALIZER;

// defined at the bottom of the file, but bt_available() needs it above
static bool run_ok(const char *cmd, char *out, int len);

const char *bt_available_why(void) { return m_why; }

bool bt_available(void) {
    if (m_avail_checked) return m_avail;
    m_avail_checked = true;
    m_why[0] = 0;

    if (access("/usr/bin/bluealsa", X_OK) != 0) {
        snprintf(m_why, sizeof(m_why), "bluealsa is not installed");
        return false;
    }
    if (access("/usr/bin/bluetoothctl", X_OK) != 0) {
        snprintf(m_why, sizeof(m_why), "bluetoothctl is not installed");
        return false;
    }
    // The plugin puts its PCM definition in ALSA's configuration; without it
    // BlueZ has no audio profiles at all and a connect attempt comes back as
    // "br-connection-profile-unavailable".  Checked by looking for the file
    // rather than shelling out: the state is read many times a second.
    if (access("/etc/alsa/conf.d/20-bluealsa.conf", R_OK) != 0 &&
        access("/usr/share/alsa/alsa.conf.d/20-bluealsa.conf", R_OK) != 0) {
        snprintf(m_why, sizeof(m_why), "the bluealsa ALSA plugin is not installed");
        return false;
    }
    m_avail = true;
    return true;
}

int  bt_count(void) { return m_count; }
const char *bt_mac(int i)  { return (i >= 0 && i < m_count) ? m_dev[i].mac  : ""; }
const char *bt_name(int i) { return (i >= 0 && i < m_count) ? m_dev[i].name : ""; }
const char *bt_icon(int i) { return (i >= 0 && i < m_count) ? m_dev[i].icon : ""; }
bool bt_connected(int i)   { return (i >= 0 && i < m_count) ? m_dev[i].conn : false; }
bool bt_paired(int i)      { return (i >= 0 && i < m_count) ? m_dev[i].paired : false; }
const char *bt_adapter(void) { return m_adapter; }
bool bt_powered(void) { return m_powered; }

static const char *name_for(const char *mac) {
    if (!mac || !mac[0]) return "";
    for (int i = 0; i < m_count; i++)
        if (strcmp(m_dev[i].mac, mac) == 0) return m_dev[i].name;
    return "";
}

static bool conn_for(const char *mac) {
    if (!mac || !mac[0]) return false;
    for (int i = 0; i < m_count; i++)
        if (strcmp(m_dev[i].mac, mac) == 0) return m_dev[i].conn;
    return false;
}

const char *bt_out_mac(void)     { return m_out_mac; }
const char *bt_out_profile(void) { return m_out_prof; }
const char *bt_out_name(void)    { return name_for(m_out_mac); }
bool bt_out_connected(void)      { return conn_for(m_out_mac); }
const char *bt_in_mac(void)      { return m_in_mac; }
const char *bt_in_name(void)     { return name_for(m_in_mac); }
bool bt_in_connected(void)       { return conn_for(m_in_mac); }

// The old single-device view, kept because the console listing and the manual
// still talk in those terms: "headset" is both directions on one device.
const char *bt_active_mac(void)  { return m_out_mac[0] ? m_out_mac : m_in_mac; }
const char *bt_active_role(void) {
    if (m_out_mac[0] && m_in_mac[0] && strcmp(m_out_mac, m_in_mac) == 0) return "headset";
    if (m_out_mac[0] || m_in_mac[0]) return "separate";
    return "";
}
const char *bt_active_name(void) { return name_for(bt_active_mac()); }
bool bt_active_connected(void) {
    if (m_out_mac[0]) return conn_for(m_out_mac);
    if (m_in_mac[0])  return conn_for(m_in_mac);
    return false;
}

bool bt_alsa_devices(char *play, int playlen, char *cap, int caplen) {
    bool any = false;
    if (play && playlen > 0) {
        play[0] = 0;
        if (m_out_mac[0]) {
            snprintf(play, playlen, "bluealsa:DEV=%s,PROFILE=%s", m_out_mac,
                     (m_out_prof[0] ? m_out_prof : "a2dp"));
            any = true;
        }
    }
    if (cap && caplen > 0) {
        cap[0] = 0;
        if (m_in_mac[0]) {
            snprintf(cap, caplen, "bluealsa:DEV=%s,PROFILE=sco", m_in_mac);
            any = true;
        }
    }
    return any;
}

bool bt_valid_mac(const char *s) {
    if (!s || strlen(s) != 17) return false;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (s[i] != ':') return false;
        } else if (!isxdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

bool bt_valid_profile(const char *p) {
    return p && (strcmp(p, "a2dp") == 0 || strcmp(p, "sco") == 0);
}

bool bt_set_output(const char *mac, const char *profile) {
    if (mac && mac[0]) {
        if (!bt_valid_mac(mac)) return false;
        if (!bt_valid_profile(profile)) return false;
        snprintf(m_out_mac, sizeof(m_out_mac), "%s", mac);
        snprintf(m_out_prof, sizeof(m_out_prof), "%s", profile);
    } else {
        m_out_mac[0] = 0;
        m_out_prof[0] = 0;
    }
    return true;
}

bool bt_set_input(const char *mac) {
    if (mac && mac[0]) {
        if (!bt_valid_mac(mac)) return false;
        snprintf(m_in_mac, sizeof(m_in_mac), "%s", mac);
    } else {
        m_in_mac[0] = 0;
    }
    return true;
}

// "bt use MAC speaker|headset": speaker is the output only (A2DP), headset is
// both directions on that device (HFP).
bool bt_set_active(const char *mac, const char *role) {
    if (!bt_valid_mac(mac)) return false;
    if (strcmp(role, "speaker") == 0) {
        return bt_set_output(mac, "a2dp");
    }
    if (strcmp(role, "headset") == 0) {
        return bt_set_output(mac, "sco") && bt_set_input(mac);
    }
    return false;
}

void bt_clear(void) {
    m_out_mac[0] = 0;
    m_out_prof[0] = 0;
    m_in_mac[0] = 0;
}

// ---------------------------------------------------------------- the tools

// Run a command and collect its output.  The exit status matters: "this device
// is not connected" and "bluetoothctl could not be run" must not look the same,
// or a failed poll would clear every connected flag.  Nothing here is on a hot
// path: the commands that need it are the ones the user asked for.
static bool run_ok(const char *cmd, char *out, int len) {
    FILE *p = popen(cmd, "r");
    if (!p) return false;
    if (out && len > 0) {
        int n = 0, c;
        while (n < len - 1 && (c = fgetc(p)) != EOF) out[n++] = (char)c;
        out[n] = 0;
    } else {
        char buf[256];
        while (fgets(buf, sizeof(buf), p)) { /* drain */ }
    }
    return pclose(p) == 0;
}

// "Controller DC:A6:32:54:DF:63 (public)" -> the address
static void parse_adapter(const char *out, char *dst, int len) {
    const char *p = strstr(out, "Controller ");
    if (!p) return;
    p += 11;
    int i = 0;
    while (i < len - 1 && p[i] && !isspace((unsigned char)p[i])) {
        dst[i] = (char)toupper((unsigned char)p[i]);
        i++;
    }
    dst[i] = 0;
}

// "Device XX:.." -> address in mac, rest of the line in name
static bool parse_device_line(const char *line, char *mac, int maclen,
                              const char **name) {
    if (strncmp(line, "Device ", 7) != 0) return false;
    const char *p = line + 7;
    int i = 0;
    while (i < maclen - 1 && *p && !isspace((unsigned char)*p)) mac[i++] = *p++;
    mac[i] = 0;
    while (*p == ' ') p++;
    *name = p;
    return bt_valid_mac(mac);
}

static void parse_info(bt_dev_t *d) {
    char cmd[128], out[1024];
    snprintf(cmd, sizeof(cmd), "bluetoothctl info %s 2>/dev/null", d->mac);
    if (!run_ok(cmd, out, sizeof(out))) { d->paired = false; return; }
    d->paired = strstr(out, "Paired: yes") != NULL;
    if (d->conn == false && strstr(out, "Connected: yes") != NULL) d->conn = true;
    const char *p = strstr(out, "Icon: ");
    if (!p) return;
    p += 6;
    int i = 0;
    while (i < BT_ICON_LEN - 1 && p[i] && p[i] != '\n' && p[i] != '\r') {
        d->icon[i] = p[i];
        i++;
    }
    d->icon[i] = 0;
}

// Rebuild the whole list.  Costly (one bluetoothctl per device), so it is run
// when a command asks for it and by the monitor every half a minute - not by
// every state message.  The result is swapped in at the end, under the lock, so
// a reader never observes a half-built list.
void bt_refresh(void) {
    bt_dev_t tmp[BT_MAX_DEV];
    int n = 0;
    char out[8192];
    char adapter[BT_MAC_LEN] = "";
    bool powered = false;

    pthread_mutex_lock(&m_lock);

    if (run_ok("bluetoothctl show 2>/dev/null", out, sizeof(out))) {
        parse_adapter(out, adapter, sizeof(adapter));
        powered = strstr(out, "Powered: yes") != NULL;
    }

    if (run_ok("bluetoothctl devices 2>/dev/null", out, sizeof(out))) {
        for (char *line = strtok(out, "\n"); line != NULL && n < BT_MAX_DEV;
             line = strtok(NULL, "\n")) {
            char mac[BT_MAC_LEN];
            const char *name = "";
            if (!parse_device_line(line, mac, sizeof(mac), &name)) continue;
            memset(&tmp[n], 0, sizeof(tmp[n]));
            snprintf(tmp[n].mac, BT_MAC_LEN, "%s", mac);
            snprintf(tmp[n].name, BT_NAME_LEN, "%s", *name ? name : mac);
            n++;
        }
    }

    // Which of them are connected right now
    if (n > 0 && run_ok("bluetoothctl devices Connected 2>/dev/null", out, sizeof(out))) {
        for (char *line = strtok(out, "\n"); line != NULL; line = strtok(NULL, "\n")) {
            char mac[BT_MAC_LEN];
            const char *name = "";
            if (!parse_device_line(line, mac, sizeof(mac), &name)) continue;
            for (int d = 0; d < n; d++)
                if (strcmp(tmp[d].mac, mac) == 0) tmp[d].conn = true;
        }
    }

    // What kind of device each one is, and whether it is paired
    for (int d = 0; d < n; d++) parse_info(&tmp[d]);

    memcpy(m_dev, tmp, sizeof(bt_dev_t) * (size_t)n);
    m_count = n;
    snprintf(m_adapter, sizeof(m_adapter), "%s", adapter);
    m_powered = powered;

    pthread_mutex_unlock(&m_lock);
}

// The cheap half of bt_refresh(): one bluetoothctl call for all devices.  This
// is what keeps "connected" honest between full refreshes.
void bt_poll_connected(void) {
    if (!bt_available()) return;
    pthread_mutex_lock(&m_lock);
    char out[4096];
    if (run_ok("bluetoothctl devices Connected 2>/dev/null", out, sizeof(out))) {
        bool conn[BT_MAX_DEV];
        for (int i = 0; i < BT_MAX_DEV; i++) conn[i] = false;
        for (char *line = strtok(out, "\n"); line != NULL; line = strtok(NULL, "\n")) {
            char mac[BT_MAC_LEN];
            const char *name = "";
            if (!parse_device_line(line, mac, sizeof(mac), &name)) continue;
            for (int d = 0; d < m_count; d++)
                if (strcmp(m_dev[d].mac, mac) == 0) conn[d] = true;
        }
        for (int d = 0; d < m_count; d++) {
            bool changed = (m_dev[d].conn != conn[d]);
            m_dev[d].conn = conn[d];
            if (changed && (strcmp(m_dev[d].mac, m_out_mac) == 0 ||
                            strcmp(m_dev[d].mac, m_in_mac)  == 0)) {
                // A device in use has come up (or gone away).  The audio streams
                // were opened when it was not there - or are still on it - so ask
                // for a re-open; with no sound card there is nothing else to fall
                // back to, and the station must not sit silent until someone
                // restarts it.
                audio_request_reopen();
            }
        }
    }
    pthread_mutex_unlock(&m_lock);
}

// ------------------------------------------------------------- pairing

// Pair, then trust.  NoInputNoOutput is the capability that lets a "Just
// Works" speaker pair with nobody there to press a button: a one-shot
// bluetoothctl registers no agent otherwise, and BlueZ answers "No agent
// available" and the pairing never starts.  A device that insists on a PIN
// still fails, and that is reported rather than retried in a loop.
static bool pair_locked(const char *mac, int timeout_s) {
    char cmd[320], out[1024];
    snprintf(cmd, sizeof(cmd),
             "timeout %d bluetoothctl --agent NoInputNoOutput pair %s >/dev/null 2>&1",
             timeout_s, mac);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "bluetoothctl trust %s >/dev/null 2>&1", mac);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "bluetoothctl info %s 2>/dev/null", mac);
    if (!run_ok(cmd, out, sizeof(out))) return false;
    return strstr(out, "Paired: yes") != NULL;
}

bool bt_pair_sync(const char *mac, int timeout_s) {
    if (!bt_valid_mac(mac)) return false;
    pthread_mutex_lock(&m_lock);
    bool ok = pair_locked(mac, timeout_s);
    pthread_mutex_unlock(&m_lock);
    return ok;
}

void bt_pair_async(const char *mac) {
    if (!bt_valid_mac(mac)) return;
    char cmd[400];
    snprintf(cmd, sizeof(cmd),
             "( bluetoothctl --agent NoInputNoOutput pair %s >/dev/null 2>&1; "
             "  bluetoothctl trust %s >/dev/null 2>&1; "
             "  bluetoothctl connect %s >/dev/null 2>&1 ) &",
             mac, mac, mac);
    if (system(cmd) != 0) { /* the shell would not start; nothing else to try */ }
}

// Connect and wait for the answer.  An unpaired device is paired first -
// "connect" alone cannot do it from a one-shot command.
bool bt_connect_sync(const char *mac, int timeout_s) {
    if (!bt_valid_mac(mac)) return false;
    char cmd[320], out[1024];
    bool up = false;

    pthread_mutex_lock(&m_lock);
    snprintf(cmd, sizeof(cmd), "bluetoothctl info %s 2>/dev/null", mac);
    bool known  = run_ok(cmd, out, sizeof(out));
    bool paired = known && strstr(out, "Paired: yes") != NULL;
    if (!paired) pair_locked(mac, timeout_s);
    snprintf(cmd, sizeof(cmd), "timeout %d bluetoothctl connect %s >/dev/null 2>&1",
             timeout_s, mac);
    system(cmd);   // the exit status of timeout says nothing useful here
    snprintf(cmd, sizeof(cmd), "bluetoothctl info %s 2>/dev/null", mac);
    if (run_ok(cmd, out, sizeof(out)))
        up = strstr(out, "Connected: yes") != NULL;
    pthread_mutex_unlock(&m_lock);
    return up;
}

// Both of these are deliberately fire-and-forget: pairing and connecting can
// take several seconds, and the command thread that asked for them should not
// be held up by it.  The result arrives in the list (and in the audio path).
void bt_connect_async(const char *mac) {
    if (!bt_valid_mac(mac)) return;
    char cmd[420];
    snprintf(cmd, sizeof(cmd),
             "( bluetoothctl --agent NoInputNoOutput pair %s >/dev/null 2>&1; "
             "  bluetoothctl trust %s >/dev/null 2>&1; "
             "  bluetoothctl connect %s >/dev/null 2>&1 ) &",
             mac, mac, mac);
    if (system(cmd) != 0) { /* the shell would not start; nothing else to try */ }
}

void bt_scan_async(void) {
    if (system("bluetoothctl --timeout 20 scan on >/dev/null 2>&1 &") != 0) { /* as above */ }
}

// ---------------------------------------------------------------- the monitor

// Five seconds is short enough that the UI cannot sit on a stale "connected"
// after the speaker is switched off, and long enough that one bluetoothctl
// call per interval costs nothing next to the vocoder.
static void *monitor_thread(void *arg) {
    (void)arg;
    int tick = 0;
    while (true) {
        usleep(5 * 1000 * 1000);
        bt_poll_connected();
        if (++tick >= 6) {          // rebuild names and paired flags every 30 s
            tick = 0;
            bt_refresh();
        }
    }
    return NULL;
}

void bt_start_monitor(void) {
    if (!bt_available()) return;
    // Fill the list before the first state message goes out, or the UI shows
    // "nothing known yet" (and the selected devices as bare MACs) until the
    // first timer refresh half a minute later.
    bt_refresh();
    pthread_t t;
    if (pthread_create(&t, NULL, monitor_thread, NULL) == 0) pthread_detach(t);
}
