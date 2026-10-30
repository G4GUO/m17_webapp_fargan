# M17 receive-path observations, October 2026

Notes from getting AX.25 working over M17 between two stations built on this
program, for whoever is still writing the specification.  They are offered as
observed behaviour with the measurements that produced them, not as a claim
about what the specification says.

## Test setup

* Two Raspberry Pi hosts, each with an ADALM-PLUTO (AD9363), 144.450 MHz,
  M17 4FSK, both at maximum transmit gain and 51 dB of manual receive gain.
* Both running this program.  A third station — a commercial M17 radio — was
  used as a reference transmitter.
* Measured carrier offset between the two Plutos: **+469 Hz**.  Received level
  during a burst: about 0.35 of full scale, i.e. not clipping.

## Observation 1 — the receive path works for a reference signal

A voice transmission from the commercial radio decoded normally on both
receivers: source callsign shown, frame counter incrementing, audio passed to
the speaker.  The demodulator, de-interleaver, Viterbi decoder and LSF check
are therefore working on a signal produced by an independent implementation.

## Observation 2 — the last symbols of every frame are lost

A burst transmitted by this program decodes with the **first 24 bytes of the
30-byte link setup frame perfect and the last 6 bytes wrong**, every time, in
every copy within a burst.  The last 24 symbols of a 192-symbol frame are
affected, consistently.

Evidence, from the receiver's own decode of a frame whose transmitted bytes were
also logged at the sender:

```
tx lsf : ffffffffffff 001c8c47229f 0000 00..00 edc5
rx lsf : ffffffffffff 001c8c47229f 0000 00..00 20afce15 2668
```

The sender's frame is provably correct: CRC-16 (polynomial 0x5935, init 0xFFFF)
over the first 28 bytes is `0xedc5`, which is what was appended, so the residual
over all 30 bytes is zero.  The receiver recovers bytes 0–23 and loses the rest.

Because the corrupted region is the end of the frame, and the CRC lives there,
the link setup frame **never** validates, and packet mode therefore never
delivers anything on this path.

A further symptom of the same defect: the end-of-transmission frame is never
received.  This was noticed during early development of this program and put
down to a quirk; it is the same missing tail.

## Observation 3 — a blank carrier false-locks the sync detector

A burst that begins with 40 ms of unmodulated carrier, as this program's voice
path does, is decoded by the receiver as a link setup frame of thirty `0xff`
bytes (CRC `0x4816`, which is simply the CRC of that constant).  The correlator
appears to match a sync word against a constant symbol stream.

Practical consequence: a blank carrier before the preamble is worse than no
carrier at all, and a receiver that reports "link setup received" for a carrier
is reporting a false positive.

## Observation 4 — stream mode survives what packet mode cannot

Stream frames carry the link setup five bytes at a time, Golay-protected, across
six frames, and repeat it continuously; the payload has rate-1/2 convolutional
coding and interleaving.  With that, the link setup validates
(`stored_lsf_crc = 0x0000`) even though the same tail loss is present, because
the LSF bytes are recovered from several frames and the LSF sits at the start of
each frame.

Interleaving does, however, spread the tail damage through the convolutionally
coded payload: frame numbers decoded as `65535` instead of 0, 1, 2…

## Observation 5 — zero tail frames flush the decoder

Appending two to four stream frames of zero payload at the end of the burst
makes the payload decode correctly — frame numbers become sane and the block CRC
passes.  Working hypothesis: the decoder's trellis is left unterminated when the
carrier drops straight after data, and trailing zero symbols both terminate the
code and give the demodulator settled symbols to finish on.

This is the difference between a link that never carries a byte and one that
completes AX.25 connections in both directions.

## Where to look next

Two threads, in this order.

**1. Initial sync detection.**  What we see is a burst that loses sync without
ever reporting a source callsign, which is a preamble (sync word) detection
failure rather than a decode failure: if the correlator had locked onto the LSF
sync, the callsign would appear even if the payload CRC then failed.  Worth
establishing:

* what the sync correlation threshold and the `variance < 0.5` test in
  `m17_rx_frame.cpp` actually admit, and whether a constant or alternating
  symbol stream can pass them (a blank carrier already does — observation 3);
* whether the receiver re-arms its sync search per frame or only at the frame
  clock boundary, which decides whether one false lock costs the rest of the
  burst;
* how many symbols of preamble the correlator needs before it will commit, and
  what happens if the burst is shorter than that.

**2. Packet frame handling.**  Packet mode is the specified way to carry data,
and we have not yet been able to exercise it because its LSF never survived the
tail loss.  Before writing it off, check the implementation on both sides
against the specification: `m17_fmt_add_packet()` / `m17_send_packet_frames()`
on transmit (25-byte chunks, EOF bit, frame counter versus valid-byte count on
the final frame) and `parse_packet()` on receive, which currently indexes chunks
by `fn*25` and trusts the byte count in the metadata.  If the receive tail loss
is fixed, packet mode should be re-tested rather than assumed broken.

## Suggestions for the specification

1. State the requirement at the end of a transmission explicitly.  If a
   receiver needs trailing symbols to decode the last frame correctly, say so,
   and say how many — the four flush bits of the convolutional code are not the
   same thing as symbols the demodulator needs.
2. Say whether an unmodulated carrier may precede the preamble.  If it may, a
   receiver must not treat it as a frame.
3. Note that packet mode is single-shot: one link setup frame per transmission,
   no repetition and no interleaving, so it needs a markedly cleaner channel
   than a voice over.  Stream mode's repeated LSF is the difference between the
   two, and it is worth stating that a packet implementation may reasonably
   repeat its link setup.
4. Publish measured deviation and occupied bandwidth for a reference
   transmitter, so an implementation can check its own modulator — a signal can
   sound perfect on an FM receiver and still fail to decode.
