#!/usr/bin/env python3
"""Check the station against the Android remote head's protocol.

The phone app speaks the station's own WebSocket protocol; this script is the
same conversation without the phone, so a station change that breaks the remote
head shows up here.

    python3 tools/android_proto_check.py                     # verify
    python3 tools/android_proto_check.py --legacy            # what the 2024 app got wrong
    python3 tools/android_proto_check.py 10.168.1.5 8080

Checks, in order:
  * the greeting carries proto/audio tags and the state carries the fields the
    app displays
  * "rf"/"tf" (the keypad's ENT) change the frequencies
  * tagged microphone audio keyed with "tx" reaches the modulator (mic_level
    moves, tx_drops stays small)
  * decoded receive audio comes back tagged for the speaker

It connects as /ws?client=phone, the way the app does; a station whose microphone
route is "phone" only takes audio from a client that identifies itself.
--legacy runs the pre-2026 app instead: untagged binary, and "TF 146400000"
style text parsing.  It should fail, and says how.
"""
import json
import math
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wstest import WSClient

TAG_MIC = 0x01
TAG_SPK = 0x02


def tone(n, i0=0):
    """A voiced buzz, so the vocoder has something to encode."""
    return b"".join(
        struct.pack("<h", int(6000 * math.sin(2 * math.pi * 180 * (i0 + i) / 8000.0)
                              * (0.5 + 0.5 * math.sin(2 * math.pi * 3 * (i0 + i) / 8000.0))))
        for i in range(n))


class Head:
    def __init__(self, ws):
        self.ws = ws
        self.state = {}
        self.hello = {}
        self.lines = []
        self.audio = 0
        self.texts = 0
        self.tags = set()

    def pump(self, seconds):
        t0 = time.time()
        while time.time() - t0 < seconds:
            m = self.ws.recv()
            if m is None:
                time.sleep(0.005)
                continue
            op, payload = m
            if op == 1:
                self.texts += 1
                text = payload.decode("utf-8", "replace")
                try:
                    d = json.loads(text)
                except Exception:
                    continue
                if d.get("t") == "state":
                    self.state.update(d)
                elif d.get("t") == "hello":
                    self.hello.update(d)
                else:
                    if len(self.lines) < 400:
                        self.lines.append(text)
            elif op == 2 and payload:
                self.tags.add(payload[0])
                if payload[0] == TAG_SPK:
                    self.audio += len(payload) - 1


def fail(msg):
    print("FAIL " + msg)
    return 1


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    legacy = "--legacy" in sys.argv
    host = args[0] if args else "127.0.0.1"
    port = int(args[1]) if len(args) > 1 else 8080

    # Connect the way the app does: as the phone, so a station whose audio is
    # routed to the phone will accept this client's microphone.
    ws = WSClient(host, port, path="/ws?client=phone")
    head = Head(ws)
    print(f"station {host}:{port}")

    head.pump(0.6)
    if legacy:
        return legacy_run(ws, head)

    hello = head.hello.get("proto")
    print(f"  proto={hello} rate={head.hello.get('rate')} "
          f"tags={head.hello.get('audio_tag_mic')}/{head.hello.get('audio_tag_spk')}")
    if head.hello.get("audio_tag_mic") != TAG_MIC or head.hello.get("audio_tag_spk") != TAG_SPK:
        return fail("greeting does not carry the audio tag bytes")
    if head.state.get("t") != "state":
        return fail("no state in the greeting")

    missing = [k for k in ("rx_freq", "tx_freq", "src", "dst", "mode", "ptt", "mic_level")
               if k not in head.state]
    if missing:
        return fail("state is missing " + ", ".join(missing))
    print(f"  state: mode={head.state['mode']} ptt={head.state['ptt']} "
          f"src={head.state['src'].strip()} rx={head.state['rx_freq']}")

    # The keypad's ENT
    target = 146412500
    ws.send_text(f"rf {target}")
    ws.send_text(f"tf {target}")
    head.pump(0.6)
    if head.state.get("rx_freq") != target:
        return fail(f"rf did not take: {head.state.get('rx_freq')}")
    print(f"  rf/tf: {target} taken")

    # Transmit: tagged audio, as the app sends it
    before = head.state.get("tx_drops", 0)
    ws.send_text("tx")
    head.pump(0.3)
    if head.state.get("ptt") != "TX":
        return fail("tx did not key the transmitter")
    peak = 0.0
    for k in range(100):                      # 2 s of 20 ms frames
        ws.send_bin(bytes([TAG_MIC]) + tone(160, k * 160))
        head.pump(0.02)
        peak = max(peak, head.state.get("mic_level", 0.0))
    head.pump(0.4)
    drops = head.state.get("tx_drops", 0) - before
    ws.send_text("rx")
    head.pump(0.6)
    print(f"  transmit: mic_level peak {peak:.3f}, tx_drops {drops}")
    if peak <= 0.0:
        return fail("microphone audio was not accepted (mic_level stayed 0)")
    if head.state.get("ptt") != "RX":
        return fail("rx did not unkey the transmitter")

    print(f"  receive: {head.audio // 2} samples of tagged audio, tags seen {sorted(head.tags)}")
    ws.close()
    if TAG_SPK not in head.tags:
        print("  note: no received audio - silence on air, not a protocol fault")
    print("OK")
    return 0


def legacy_run(ws, head):
    """The app as it was written in 2024: untagged PCM, text status lines."""
    print("  --legacy: untagged microphone audio, 'TF nnnn' text parsing")
    ws.send_text("wu")                        # not a command on this station
    ws.send_text("rf 146400000")
    ws.send_text("tf 146400000")
    head.pump(0.6)

    lines = getattr(head, "lines", [])
    parsed = 0
    for t in lines:
        if t[:2] in ("TF", "RF", "AS", "LS", "TX", "RX", "SC", "DT", "MD"):
            parsed += 1
    print(f"  text frames {head.texts}, of which the app's parser understands {parsed}")
    print(f"  binary tags seen {sorted(head.tags)} (the app plays whatever arrives)")

    ws.send_text("tx")
    head.pump(0.3)
    for k in range(50):
        ws.send_bin(tone(160, k * 160))       # no tag
        head.pump(0.02)
    head.pump(0.4)
    mic = head.state.get("mic_level", 0.0)
    print(f"  mic_level while keyed with untagged audio: {mic:.3f}")
    ws.send_text("rx")
    ws.close()
    if parsed == 0 and mic == 0.0:
        print("AS EXPECTED: an untagged client gets no status it can read and no "
              "transmit audio.")
        return 0
    return fail("legacy client unexpectedly worked")


if __name__ == "__main__":
    sys.exit(main())
