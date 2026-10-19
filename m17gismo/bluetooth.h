// bluetooth.h
//
// Bluetooth audio for m17web.
//
// A Bluetooth speaker or headset is not a sound card.  On Linux the audio
// profiles have to be provided by a backend - BlueALSA here, since this
// station deliberately has no PulseAudio or PipeWire - which then presents
// them to ALSA as ordinary PCM devices:
//
//     bluealsa:DEV=F4:4E:FD:5D:F9:A1,PROFILE=a2dp    A2DP, playback only
//     bluealsa:DEV=E8:07:BF:39:62:F9,PROFILE=sco     HFP, both directions
//
// so the existing ALSA back end can use them unchanged.  What this module adds
// is choosing one of the devices Bluetooth knows about and putting it into the
// audio path.  It is entirely optional: with no bluealsa and no bluetoothctl
// the station behaves exactly as before and the UI says why.
//
// Two profiles matter, and they are not interchangeable on one device:
//
//   speaker  A2DP out.  Best audio, but playback only and a good 100-200 ms of
//            latency, so the microphone stays on the sound card.
//   headset  HFP in and out.  Lower quality at 8 kHz mono - which is exactly
//            M17's own rate, so nothing is lost relative to Codec2's 3.2 kbps.
#ifndef BLUETOOTH_H
#define BLUETOOTH_H

#include <stdbool.h>

#define BT_MAX_DEV  12
#define BT_MAC_LEN  18
#define BT_NAME_LEN 40
#define BT_ICON_LEN 24

// Is there a usable Bluetooth audio backend on this host (bluealsa installed
// and running, bluetoothctl present)?  Checked once, then remembered.
bool bt_available(void);
const char *bt_available_why(void);        // "" when it is available

// The cached list of devices Bluetooth knows about.  Refreshed by bt_refresh(),
// which shells out, so it is only ever called when a command asks for it - the
// state is sent many times a second and must not run anything.
void bt_refresh(void);
int  bt_count(void);
const char *bt_mac(int i);
const char *bt_name(int i);
const char *bt_icon(int i);
bool bt_connected(int i);
bool bt_paired(int i);                     // "Paired: yes" for that device

const char *bt_adapter(void);              // "DC:A6:32:54:DF:63", "" if none
bool bt_powered(void);

// The two selections, kept apart: output can be an A2DP speaker while the
// microphone stays on the sound card (or on a headset).  An empty MAC means the
// sound card for that direction.
const char *bt_out_mac(void);              // "" = the sound card
const char *bt_out_profile(void);          // "a2dp" | "sco"
const char *bt_out_name(void);
bool bt_out_connected(void);
const char *bt_in_mac(void);               // "" = the sound card
const char *bt_in_name(void);
bool bt_in_connected(void);

// The old single-device view: "headset" is both directions on one device.
const char *bt_active_mac(void);
const char *bt_active_role(void);          // "headset" | "separate" | ""
const char *bt_active_name(void);
bool bt_active_connected(void);

// ALSA device strings for the two selections.  Each is filled in only when that
// direction is on Bluetooth; an empty string means "whatever card is present".
bool bt_alsa_devices(char *play, int playlen, char *cap, int caplen);

// Choose a direction.  A NULL/empty MAC puts that direction back on the card.
bool bt_set_output(const char *mac, const char *profile);   // profile: a2dp|sco
bool bt_set_input(const char *mac);
bool bt_valid_profile(const char *p);

// Remember a choice without touching any hardware: used by "bt spk"/"bt mic"
// after they have asked for the connection, and by config.txt at startup.
bool bt_set_active(const char *mac, const char *role);      // speaker|headset
void bt_clear(void);

// Both of these run in the background so the command that asked for them
// returns at once; the outcome shows up in the cached list and in the device's
// connected flag.
void bt_connect_async(const char *mac);
// Connect and wait for it (up to timeout_s).  Pointing the audio at a device
// whose A2DP PCM does not exist yet just falls back to the card, and bluealsa
// only creates the PCM once the profile is up - so a command that routes audio
// to a device has to let the connection finish first.  An unpaired device is
// paired first; "connect" alone cannot do that from a one-shot command.
bool bt_connect_sync(const char *mac, int timeout_s);
void bt_scan_async(void);

// Pairing.  A device found by a scan is only *discovered*: BlueZ will not
// connect it until it is paired, and a one-shot bluetoothctl registers no
// pairing agent unless asked.  bt_pair_sync() pairs with a NoInputNoOutput
// agent (enough for a speaker's "Just Works") and trusts the device; a device
// that demands a PIN fails and says so.  Synchronous so the command can report
// what happened; the asynchronous form pairs, trusts and connects.
bool bt_pair_sync(const char *mac, int timeout_s);
void bt_pair_async(const char *mac);

// Start the background monitor: it keeps the connected flag honest (a speaker
// that is switched off must not keep showing "connected") and rebuilds the
// device list every half minute.  A no-op where Bluetooth is unavailable.
void bt_start_monitor(void);
void bt_poll_connected(void);              // one cheap poll, also callable by hand

// A strictly formatted MAC address.  These strings end up in a shell command
// line, so nothing else is accepted.
bool bt_valid_mac(const char *s);

#endif // BLUETOOTH_H
