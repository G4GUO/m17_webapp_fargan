#!/bin/sh
# record_check.sh - exercise the audio recorder on a running station.
#
#     sh tools/record_check.sh [working-directory]
#
# Starts a recording, shows the state and the file it created, stops it, and then
# checks that file with Python's wave module.  A strict parser is the point: a
# WAV header that is wrong by one field still looks like a plausible file size,
# which is exactly how one got through before.
HERE=$(cd "$(dirname "$0")/.." && pwd)
WS=$HERE/m17gismo/tools/wstest.py
PORT=${PORT:-8080}
DIR=${1:-/home/charles/m17web-fargan}

echo "=== record on ==="
python3 "$WS" --port "$PORT" --cmd "record on" --steps 3 --quiet 2>&1 | grep -i resp | tail -2
sleep 2
ls -l "$DIR"/rx-*.wav 2>/dev/null || echo "(no file yet)"

echo
echo "=== the state's record field ==="
python3 "$WS" --port "$PORT" --cmd st --steps 2 --quiet --state-out /tmp/rec.json >/dev/null 2>&1
tr ',' '\n' < /tmp/rec.json | grep -iE "record|file|seconds" | head -8

echo
echo "=== record off ==="
python3 "$WS" --port "$PORT" --cmd "record off" --steps 3 --quiet 2>&1 | grep -i resp | tail -1

echo
echo "=== that file, checked with a strict parser ==="
python3 - "$DIR" <<'PY'
import glob, os, sys, wave
d = sys.argv[1]
files = sorted(glob.glob(os.path.join(d, "rx-*.wav")), key=os.path.getmtime)
if not files:
    print("no rx-*.wav in", d)
    raise SystemExit
p = files[-1]
try:
    w = wave.open(p, "rb")
    print("%s: %d ch, %d-bit, %d Hz, %.2f s, %d frames, %d bytes on disk"
          % (os.path.basename(p), w.getnchannels(), w.getsampwidth() * 8, w.getframerate(),
             w.getnframes() / w.getframerate(), w.getnframes(), os.path.getsize(p)))
except Exception as e:
    print("NOT PLAYABLE:", e)
PY
