#!/bin/sh
# fargan_selftest.sh - check the neural receiver inside the station, with no
# radio, no sound card and no reflector involved.
#
#     sh tools/fargan_selftest.sh [path/to/m17web]
#
# It starts a separate instance of the station in a scratch directory - with its
# own config.txt, so the real station's settings are never touched - driving the
# simulated radio in duplex with the file audio back end.  The simulator
# receives what it transmits, so the receive path (the only path the neural
# receiver changes) is exercised for real, through the same code an RF or
# reflector frame takes.
#
# Two passes are taken, FARGAN off and then on, and the two decoded streams are
# reported side by side.  Nothing is transmitted and the sound card is not
# opened.
HERE=$(cd "$(dirname "$0")/.." && pwd)
BIN=${1:-$HERE/m17gismo/m17web}
WS=$HERE/m17gismo/tools/wstest.py
DIR=${TMPDIR:-/tmp}/fargan_selftest
PORT=${PORT:-8099}
IN=${IN:-$HOME/codec2/raw/hts2a.raw}
SECS=${SECS:-6}

[ -x "$BIN" ] || { echo "no binary at $BIN - build it first (make LIME=1 SIM=1 CODEC2_BACKEND=mod)"; exit 1; }
[ -f "$IN" ]  || { echo "no test vector at $IN - set IN=/path/to/8k.raw"; exit 1; }

mkdir -p "$DIR" || exit 1
cd "$DIR" || exit 1
rm -f config.txt classic.raw fargan.raw out.raw log.txt
echo "fargan off" > config.txt      # start from the classic decoder, as shipped

start() {
    rm -f out.raw
    "$BIN" -r sim -w "$PORT" -a "file:$IN:$DIR/out.raw" > log.txt 2>&1 &
    echo $! > pid
    sleep 3
}
stop() {
    kill "$(cat pid)" 2>/dev/null
    sleep 1
}

echo "=== pass 1: the classic decoder ==="
start
python3 "$WS" --port "$PORT" --cmd "mode radio" --cmd "fargan" --steps 3 --quiet 2>&1 | grep -i resp | tail -3
python3 "$WS" --port "$PORT" --ptt --duration "$SECS" --quiet >/dev/null 2>&1
stop
[ -s out.raw ] && cp -f out.raw classic.raw || echo "  (nothing decoded in pass 1)"

echo
echo "=== pass 2: the neural receiver ==="
start
python3 "$WS" --port "$PORT" --cmd "mode radio" --cmd "fargan on" --steps 3 --quiet 2>&1 | grep -i resp | tail -3
python3 "$WS" --port "$PORT" --ptt --duration "$SECS" --quiet >/dev/null 2>&1
stop
[ -s out.raw ] && cp -f out.raw fargan.raw || echo "  (nothing decoded in pass 2)"

echo
echo "=== config.txt now says ==="; cat config.txt

echo
echo "=== the two decodes ==="
python3 - "$DIR" <<'PY'
import struct, math, os, sys
d = sys.argv[1]
def load(p):
    b = open(os.path.join(d, p), 'rb').read()
    n = len(b) // 2
    return list(struct.unpack('<%dh' % n, b[:n * 2])), n
def rms(s):
    return math.sqrt(sum(v * v for v in s) / len(s)) / 32768.0 if s else 0.0
def env(s, w=160):
    return [rms(s[i:i + w]) for i in range(0, len(s) - w + 1, w)]
for p in ('classic.raw', 'fargan.raw'):
    f = os.path.join(d, p)
    if not os.path.exists(f) or os.path.getsize(f) == 0:
        print('%-12s missing or empty' % p); continue
    s, n = load(p)
    print('%-12s %6d samples (%5.2f s of 8 kHz), RMS %.4f' % (p, n, n / 8000.0, rms(s)))
try:
    a, _ = load('classic.raw'); b, _ = load('fargan.raw')
    ea, eb = env(a), env(b)
    m = min(len(ea), len(eb))
    if m > 4:
        ma, mb = sum(ea[:m]) / m, sum(eb[:m]) / m
        num = sum((ea[i] - ma) * (eb[i] - mb) for i in range(m))
        den = math.sqrt(sum((ea[i] - ma) ** 2 for i in range(m)) *
                        sum((eb[i] - mb) ** 2 for i in range(m)))
        print('envelope correlation between the two decodes: %.3f over %d frames' % (num / den if den else 0.0, m))
except Exception as e:
    print('comparison not possible:', e)
PY
echo
echo "=== the station's own log, last lines ==="; tail -5 log.txt
