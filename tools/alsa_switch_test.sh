#!/bin/sh
# alsa_switch_test.sh - does switching the sound device leak a PCM, and does the
# station end up reporting "Device or resource busy" against itself?
#
#     sh tools/alsa_switch_test.sh [bluetooth-mac]
#
# It cycles the audio device - the USB card, the Pi's headphone jack, back and
# forth, optionally the Bluetooth speaker - and after each switch reports both
# what the station says and which PCMs the process actually holds.  A held PCM
# with no open stream is the leak this is looking for; a device that stays busy
# across switches is the symptom Charles hit.
#
# The Pi's headphone jack is playback-only, so the microphone is expected to stay
# on the USB card when it is chosen - the station's capture fallback reports that
# in the response, and it is not a failure.
HERE=$(cd "$(dirname "$0")/.." && pwd)
WS=$HERE/m17gismo/tools/wstest.py
PORT=${PORT:-8080}
DEV1=${DEV1:-plughw:3,0}      # USB sound card (playback and capture)
DEV2=${DEV2:-plughw:2,0}      # Pi headphone jack (playback only)
BT=${1:-}

check() {
    python3 "$WS" --port "$PORT" --cmd st --steps 2 --quiet --state-out /tmp/sw.json >/dev/null 2>&1
    printf '    state: '
    tr ',' '\n' < /tmp/sw.json |
        grep -iE '"audio"|"device"|"error"|spk_open|mic_open' | tr -d ' ' | tr '\n' ' '
    echo
    printf '    PCMs held: '
    for p in /dev/snd/pcmC*D*p /dev/snd/pcmC*D*c; do
        [ -e "$p" ] || continue
        printf '%s=' "$(basename "$p")"
        sudo fuser "$p" 2>/dev/null | tr '\n' ',' || printf 'none'
    done
    echo
}

switch_to() {
    echo "--- $1 ---"
    python3 "$WS" --port "$PORT" --cmd "$1" --steps 3 --quiet 2>&1 | grep -i resp | tail -2
    sleep 4
    check
}

echo "=== as found ==="; check
switch_to "audio alsa $DEV2"
switch_to "audio alsa $DEV1"
switch_to "audio alsa $DEV2"
switch_to "audio alsa $DEV1"
[ -n "$BT" ] && switch_to "bt use $BT speaker"
echo
echo "=== busy / fallback / failure lines seen while switching ==="
grep -iE "busy|not available|failed" "$HERE/run.log" 2>/dev/null | tail -10 || echo "(no run.log in $HERE)"
