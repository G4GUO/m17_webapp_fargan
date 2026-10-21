#!/usr/bin/env python3
"""A minimal M17 reflector, for testing client mode without the internet.

    python3 tools/fake_reflector.py 17000 G4ABC

Put "LOCAL 127.0.0.1 17000" in M17Hosts.txt, start the station in client mode
with "conn LOCAL C" (or "conn 127.0.0.1 C" style config), and the box above PUSH
TO TALK should say "hearing G4ABC" while the frames are arriving, then go back to
"no signal" about a second and a half after this is killed.  It answers CONN with
ACKN and PING with PONG, and streams 25 voice frames a second for 20 s after a
connect, so the receive path, the address and the loss-of-signal timeout can all
be exercised with no radio and no internet.

Speaks just enough of the protocol: answers CONN with ACKN, answers PING with
PONG, and then streams voice frames from a made-up callsign at 25 frames/s so the
station has something to show in its "hearing ..." box.  Silence is sent every
few seconds so the loss-of-signal path can be seen too.

    python3 fake_reflector.py <port> <callsign>
"""
import socket
import struct
import sys
import time

CRC_POLY = 0x5935


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ CRC_POLY) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode_call(call: str) -> int:
    call = (call.upper() + "         ")[:9]
    words = []
    for ch in call:
        if 'A' <= ch <= 'Z':
            words.append(ord(ch) - ord('A') + 1)
        elif '0' <= ch <= '9':
            words.append(ord(ch) - ord('0') + 27)
        elif ch == '-':
            words.append(37)
        elif ch == '/':
            words.append(38)
        elif ch == '.':
            words.append(39)
        else:
            words.append(0)
    value = 0
    # The station decodes the least significant digit into the first character
    # (m17_decode_call: call[8-i] = word % 40), so the padding goes in first.
    for w in reversed(words):
        value = value * 40 + w
    return value


def pack48(value: int) -> bytes:
    return value.to_bytes(6, 'big')


def make_frame(stream: int, dst: int, src: int, payload: bytes) -> bytes:
    frame = bytearray()
    frame += b'M17 '
    frame += struct.pack('>H', stream & 0xFFFF)
    frame += pack48(dst)
    frame += pack48(src)
    frame += b'\x00\x00'      # type: voice, no encryption
    frame += b'\x00' * 14     # meta
    frame += b'\x00\x00'      # frame number
    frame += payload[:16].ljust(16, b'\x00')
    # The station accepts a frame when the CRC over all 54 bytes is zero, so
    # pick the trailing bytes that make that true.
    c = crc16(bytes(frame))
    for tail in (struct.pack('>H', c), struct.pack('<H', c),
                 struct.pack('>H', (~c) & 0xFFFF), struct.pack('<H', (~c) & 0xFFFF)):
        if crc16(bytes(frame) + tail) == 0:
            return bytes(frame) + tail
    raise SystemExit('could not make the CRC work out')


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 17000
    call = sys.argv[2] if len(sys.argv) > 2 else 'G4ABC'
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('127.0.0.1', port))
    sock.settimeout(0.5)
    print(f'fake reflector on 127.0.0.1:{port}, streaming as {call}', flush=True)
    peer = None
    stream = 1
    src = encode_call(call)
    dst = encode_call('ALL')
    talking_until = 0.0
    next_frame = 0.0
    while True:
        try:
            data, addr = sock.recvfrom(1024)
            peer = addr
            tag = data[:4]
            if tag == b'CONN':
                mod = chr(data[10]) if len(data) > 10 else 'C'
                print(f'CONN from {addr} module {mod} -> ACKN', flush=True)
                sock.sendto(b'ACKN', addr)
                talking_until = time.time() + 20     # pretend somebody is talking
                stream += 1
            elif tag == b'PING':
                sock.sendto(b'PONG' + data[4:10], addr)
            elif tag == b'DISC':
                print('DISC', flush=True)
                sock.sendto(b'DISC', addr)
        except socket.timeout:
            pass
        now = time.time()
        if peer and now < talking_until and now >= next_frame:
            # 16 bytes of payload: two codec2 frames.  Silence is fine - the
            # station decodes it, and what is being tested here is the address
            # and the in-frame indication, not the audio.
            sock.sendto(make_frame(stream, dst, src, b'\x00' * 16), peer)
            next_frame = now + 0.04


if __name__ == '__main__':
    main()
