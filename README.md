# m17_sdr_web / m17web

Web-UI fork of [G4GUO/m17_sdr](https://github.com/G4GUO/m17_sdr): the same M17
SDR implementation, but the ncurses terminal interface has been replaced by a
browser interface.  The M17 protocol, DSP, framing and network code is
upstream's own; everything front-end related is new.

    Host (Windows / Linux)                     Raspberry Pi 4 + SDR
    +------------------------+   WebSocket   +---------------------------+
    | browser UI             |<------------->| m17web                    |
    |  . PTT + meters        |  JSON state   |  . HTTP + WS server       |
    |  . frequencies/gains   |  PCM audio    |  . M17 modem (unchanged)  |
    |  . reflector control   |               |  . Pluto / Lime / SIM     |
    |  . console + log       |               |  . Codec2 3200 voice      |
    +------------------------+               +---------------------------+

**User manual:** [docs/m17web-user-manual.pdf](docs/m17web-user-manual.pdf) — a 12-page guide written for the operator rather than the developer: setting it up, making a contact, the audio options, the controls, the commands, troubleshooting and a quick reference. The text lives in `docs/user_manual.py`, which regenerates the PDF (needs `reportlab`).

## What changed relative to upstream

| Area | Upstream | Here |
|------|----------|------|
| User interface | ncurses (`gui.cpp`, stdin command loop in `main.cpp`) | `webui.cpp` + `webserver.c` + `www/` single page app |
| Console UI | only option | still available: `make console` (`ui_console.cpp`, same API) |
| Voice audio | PulseAudio on the Pi | browser mic/speaker (WebAudio) by default; `pulse`, `both`, `file`, `none` selectable at run time |
| Radio | Pluto / LimeSDR | plus a **simulated radio** (`radio_sim.cpp`) so everything runs with no hardware |
| UI boundary | `gui_*()` calls scattered through the code | same names, now declared in `ui.h` and implemented by either front end |
| GPS | NMEA reader in `gps.cpp`: hard-coded `/dev/ttyACM0`, no port speed, pre-2.0 GNSS encoder, result unused | device resolved by identity under `/dev/serial/by-id`, raw 38400 set with termios, Maidenhead locator, specification 2.0.9 META payload, received positions decoded and shown |
| Binary | `m17gismo` | `m17web` |

`gps.cpp` is the exception: upstream's reader opened a hard-coded
`/dev/ttyACM0` - which on a machine with a PlutoSDR attached is the Pluto's
serial console, not the receiver - never set the port speed, and its GNSS
encoder predated the specification 2.0 layout.  It has been rewritten; see
"GPS: the station's position, and the Maidenhead locator" below.

## The neural receiver (FARGAN)

An optional, **receive-only** upgrade to the voice decoder.  M17 carries Codec2
3200, and [Codec2-mod-neural](https://github.com/M17-Project/Codec2-mod-neural)
by **Wojciech Kaczmarski SP5WWP** replaces only the *synthesis* step: the same
8-byte frames, decoded by FARGAN (the vocoder from Opus) at 16 kHz instead of by
the sinusoidal model at 8 kHz.  The bitstream, the transmitter and what other
stations receive are untouched, so a station can have this for itself without
anyone else changing anything - and it is not infectious: to hear it, the
station receiving has to have it.

    fargan          is it built in, and is it on
    fargan on       receive through FARGAN (kept in config.txt)
    fargan off      the classic decoder

There is a checkbox in the Audio card as well.  It is compiled in with
`NEURAL=1` (the default; needs `CODEC2_BACKEND=mod`) and left out entirely with
`NEURAL=0`.  It starts **off**, and a station's choice is kept in `config.txt`.

It is not free.  Measured on this station's Pi 4B: **9.6 ms per 20 ms frame -
48 % of one core, 2.1x real time** - against 0.089 ms (0.44 %) for the classic
decoder, so on a Pi it is a deliberate choice rather than a default.  The 320
samples FARGAN produces are decimated back to the audio path's 160, which costs
0.017 ms per frame.  The harness, the raw runs and the integration notes are in
`/mnt/hf/shared/projects/m17_neural` (`RESULTS.md`, `docs/PLAN.md`).

## Recording what you hear

A recording captures exactly what the receiver decodes - the samples handed to
the speaker, whichever decoder produced them - as a mono 8 kHz WAV in the
station's working directory, which is the format Codec2 3200 already works in,
so nothing has to be converted.

    record            is it recording, and where
    record on [file]  start (default rx-<date>-<time>.wav)
    record off        close it

**The recording follows the decoder in use.**  FARGAN synthesises at 16 kHz, and
that is what a recording keeps; with the neural receiver off, the classic decoder
gives 8 kHz.  The rate is fixed when recording starts, because a WAV file has one
rate - so if FARGAN is switched during a recording the new samples are refused
and the state says why, rather than being written into a file that would play at
the wrong pitch.

The Audio card carries the same control, with the file name and the running time
next to it.  It is **not** kept in `config.txt`, deliberately: a station that
began recording itself after a reboot would fill its own disk.  It stops itself
after 30 minutes, and the WAV header is rewritten every few seconds, so a
recording interrupted by a power cut still plays.

The useful trick: switch FARGAN off, record a passage, switch it on, record the
same kind of thing again.  Both files come off the identical receive path, so
the comparison is the vocoder and nothing else.

Recordings made from the live station are kept in `examples/`: the 16 kHz files
are FARGAN's own output, the 8 kHz ones the classic decoder, and the pair
recorded ten seconds apart is the closest thing to a like-for-like comparison in
the set.  Play them rather than reading any number about them.

Nothing in `m17_*.cpp`, `pluto.cpp`, `lime.cpp`, `buffers.cpp` was rewritten for
the web UI.  Small additive changes only: a reflector-module
getter, gain/name/type getters, a `radio_keyed()` fix (upstream always called
the Lime function, so a Pluto station could never read its own PTT input), and
compile guards so back ends are selected at build time.

## Building

Needs gcc/g++, make, **Codec2** built from source, and whichever of libiio
(Pluto), LimeSuite (LimeSDR), libpulse-simple (local audio) you have:

    sudo apt install libiio-dev limesuite liblimesuite-dev libpulse-dev

    cd m17_sdr_web/m17gismo
    make LIME=1 SIM=1 CODEC2_BACKEND=mod   # what the station runs
    make sim            # no SDR hardware, no PulseAudio: web UI + simulator
    make console        # legacy ncurses interface

Build options: `PULSE=0 PLUTO=0 LIME=0 SIM=1 CONSOLE=1 WEB=0 EMBED=0 DEBUG=1`,
`CODEC2_PREFIX=/usr/local`.

**The defaults do not link on Debian.**  `SIM` defaults to 0 and
`CODEC2_BACKEND` to `lib`, but `mmi.cpp` and `m17_tx_rx.cpp` reference `sim_*`
and `codec2m_*` unconditionally, so a bare `make` fails with `undefined
reference to sim_set_noise` / `codec2m_encode`.  And `LIME` is auto-detected
from `LimeSuite.h` in one of three paths, none of which is where Debian's
`liblimesuite-dev` actually installs it (`/usr/include/lime/*.h`), so the Lime
back end is silently left out and `-r lime` becomes an option that cannot work.
Hence the explicit line above.  Note `lime.cpp` includes the vendored
`LimeSuite.h` from this directory, which is ABI-compatible with the packaged
`libLimeSuite.so.23.11` - the link resolves cleanly.

Codec2 itself (no cmake on Raspberry Pi OS by default):

    sudo apt install cmake
    git clone https://github.com/drowe67/codec2.git && cd codec2
    mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j4
    sudo make install && sudo ldconfig

## Codec2 back ends: stock libcodec2 or the modified codec

Two codecs can be built in, selected with `CODEC2_BACKEND`:

    make                        # CODEC2_BACKEND=lib (default): external libcodec2
    make CODEC2_BACKEND=mod     # vendored M17-Project/Codec2-mod, no external codec

    make bitexact-check INPUT=~/codec2/raw/hts1a.raw    # compare the two

`codec2mod/` contains [M17-Project/Codec2-mod](https://github.com/M17-Project/Codec2-mod)
vendored at commit `7c334283` (see `codec2mod/VENDORED.txt`).  That fork is a
minimal extraction of **only** the 3200 bps mode - which is exactly what M17
uses - with static, caller-supplied codec state, no runtime `malloc`, and a
smaller footprint.  It ships no build system, so the makefile compiles its
sources directly, and `codec2mod/codec2_shim.c` adapts its static-state API
(`codec2_encoder_t` / `codec2_encode()`) to the upstream API the rest of the
project expects (`codec2_create(CODEC2_MODE_3200)` / `codec2_encode()`), so no
other file needed changing.  With `CODEC2_BACKEND=mod` the binary has **no
libcodec2 dependency at all** (verify with `ldd`).

Measured on the Pi 4B, same 150 frames of `hts1a.raw`, stock vs modified:

| | stock libcodec2 | modified Codec2 | ratio |
|---|---|---|---|
| encode | 0.145 ms/frame | 0.070 ms/frame | **2.07x faster** |
| decode | 0.104 ms/frame | 0.088 ms/frame | 1.18x faster |

Bitstream compatibility, which is what matters on air: **149 of 150 frames were
byte-for-byte identical**; one frame differed by 2 bits.  The fork claims
bit-exact encoder output, and on this platform that holds for 149/150 frames
here - worth knowing before relying on it for forensic comparison, but 2 bits
in one frame is irrelevant to interop.  Decoded audio was not byte-identical
(as the fork documents) but correlated 0.9999 on energy envelopes.

Both codecs were also run through the full modem path (simulator, duplex,
`file:` audio in and out): identical behaviour, `gerr=0`, decoded audio
returned in both cases.

## Running

    ./m17web -r sim -w 8080                     # simulated radio, web UI port 8080
    ./m17web -r pluto -w 8080                   # Pluto / Pluto+
    ./m17web -r lime -w 8080                    # LimeSDR
    ./m17web -r sim -a file:in.raw:out.raw      # headless voice test

    -p IP      Pluto address (default pluto.local)
    -w PORT    web UI port (default 8080)
    -www DIR   web assets (default ./www; the bundled copy is embedded in the binary)
    -a BACKEND browser (default) | pulse | both | none | file:IN.raw:OUT.raw

Open `http://<pi-address>:8080/`.  `config.txt` in the working directory is run
at startup exactly as before (callsign, mode, frequencies, ...).

Browser audio needs one click ("enable browser audio") because of autoplay
rules, and needs a secure context (localhost, https, or an explicit browser
exception) for the microphone; Firefox allows plain http, Chrome usually does
not.  The PulseAudio back end or the file back end can be used instead.

## Web interface

The layout is deliberately compact: the cards on the left are laid out two
across (the push-to-talk and Position cards span the full width), and the console
and command help are a narrow column with their own scroll bars, so a whole
session fits on one screen without the page itself scrolling.

* **PTT** - hold the button or the space bar; break / carrier / duplex buttons.
  Live RX/TX/DX indicator, signal and power meters, "in frame" badge.
* **Station** - callsign, destination, gateway, broadcast, listen-to-all.
* **Radio** - RX/TX frequency, gains, frequency correction, AFC; simulator
  controls (loopback, real time, noise) appear when running `-r sim`.
* **Reflector** - pick a reflector (list read from `M17Hosts.txt`) plus module,
  connect / disconnect, connection status.
* **Audio** - enable browser audio, choose the back end, volume, mute, and
  mic/speaker level meters.
* **Console** - every MMI command from the terminal version works here, with a
  scrolling log; `h` for help, `w` to clear.

Wire protocol (WebSocket at `/ws`): JSON text frames for state (`{"t":"state",...}`),
log lines, command responses, help text and the reflector list; binary frames
for audio, tagged `0x01` + s16le PCM (8 kHz mono) for the microphone and `0x02`
+ PCM for decoded receive audio.

## GPS: the station's position, and the Maidenhead locator

A GPS receiver plugged into the Pi is read over its serial port, shown in the web
interface, and - while the fix is fresh - broadcast in the M17 link setup frame
as the GNSS position data, so other stations can see where you are.

### Which device, and at what speed

The receiver used here is a u-blox 7 on USB.  It is a CDC-ACM device, so it
appears as `/dev/ttyACM<n>`, **not** `/dev/ttyUSB<n>`.  Two things follow:

* **The port number is not stable.**  A PlutoSDR also presents a serial port, and
  which one gets `ttyACM0` depends on what enumerated first - the same hardware
  here has been both `ttyACM0` and `ttyACM1` across reboots.  The program
  therefore resolves the receiver *by identity* under `/dev/serial/by-id/`
  (`*u-blox*`, then `*GPS*`), so the number never matters.  A hard-coded
  `ttyACM0` opens the Pluto's console instead.
* **The speed must be set explicitly.**  This receiver only emits navigation
  sentences at **38400** baud; at its default it sends its banner text and
  nothing else.  The port is set to raw 38400 with termios rather than inheriting
  whatever the driver left behind.

Override the auto-detection when needed:

    gps                      # device, speed, status, fix and locator
    gps /dev/ttyACM1 38400   # a specific device and speed
    gps meta off             # keep using the receiver, but announce nothing
    gps meta on              # announce the position again (the default)

`M17WEB_GPS=/dev/ttyACM1` does the same at startup.  If the receiver is unplugged
and plugged back in, the reader reopens it by itself.

### What is done with the fix

* **Display.**  The top bar carries the locator, the latitude/longitude, and
  whether the position is going out (`gnss: sending`).  The Position panel has
  the locator in large type (with a *copy locator* button), plus satellites,
  HDOP, altitude and the age of the fix.
* **Maidenhead locator** is computed from the fix - `IO90ST` for the position
  used while testing here - six characters: field, square, subsquare.
* **Transmitted position.**  The TYPE field's encryption subtype bits select what
  the META field carries; the program sets them to `0b01` (GNSS position) while
  the fix is fresh and clears them back to `0b00` when it is stale, so a stale
  position is never announced.  META travels in the LICH, part of the LSF that
  repeats every superframe, so the position is refreshed at superframe boundaries* **The position can be kept off the air.**  `gps meta off`, or unticking
  *announce position on air* in the Position panel, stops the position being sent
  in the META field while leaving the receiver running - the locator, the compass
  and the CSV log all still work.  The setting is kept in `config.txt`, so it
  survives a restart.  Worth knowing either way: while it is on, your position is
  public, appearing in the *Location* column of the reflector's own dashboard and
  in any client that displays META (OpenRTX shows the locator).
  `tools/gnss_meta_test.sh` proves the switch works, by looping a transmission
  back through a simulator and checking what the station hears itself announce.
  and at most once every 5 s - the rate at which the specification says a
  receiver may be expected to notice a change.
* **Received positions are decoded too:** another station sending its position
  produces a log line and appears under the Position panel as "heard ...", with
  the **bearing and distance** from your fix to theirs.  A compass shows the
  bearing to that station - the number worth having when pointing a beam - with a
  smaller second marker for the course that station reports (yellow).  The
  bearing needs a fix of your own, so it stays blank until you have one.
* **The position can be logged to CSV.**  Tick *log to CSV* in the Position panel
  (or `csv on`, `csv off`, `csv status`, `csv 10` for a different interval) and
  one row is appended every few seconds to `gps_track.csv` in the working
  directory - `/home/charles/m17web/gps_track.csv` for the service.  Set
  `M17WEB_CSV` for another path.  Columns: date, time, lat, lon, locator,
  altitude, speed, course, satellites, HDOP, age of fix, and an epoch column so
  the rows are unambiguous and sort correctly (the GPS date and time are only as
  fresh as the last sentence received).  The interface shows the file, the row
  count and whether logging is on.

### The payload: metric, per specification 2.0.9

14 bytes / 112 bits: data source and station type, a validity nibble with a 3 bit
HDOP-derived radius, a 9 bit bearing, 24 bit latitude and longitude, 16 bit
altitude in 0.5 m steps offset by 500 m, and 12 bit speed in 0.5 km/h steps.
Altitude is in **metres**: the specification changed from feet and reordered the
fields in version 2.0, so anything written against the older layout is not
compatible.  The specification describes the radius as an estimate of lateral
position uncertainty based on HDOP but defines no mapping to the 3 bit field;
`radius_code()` in `gps.cpp` uses bands of uncertainty in metres, which is a
documented approximation rather than a guess at intent.

### How this was checked

* `tests/gps_test.cpp` - known locators (including `IO90ST`), a round trip over
  all 32 400 field/square centres and 576 subsquares, and an encode/decode round
  trip of the META payload.
* `tools/gps_meta_decode.py` - decodes the META bytes from the specification
  rather than from the C, so the encoder is checked independently.
* The whole chain was then proved without hardware: the simulated radio in
  duplex mode loops the transmitter into the receiver, so the station demodulated
  its own signal and decoded the position back out of the LSF:

```
GNSS: G4GUO at 50.80371, -0.44190 IO90ST, 10 m, 0 deg, 0.5 km/h
```

To repeat it (note `td`, duplex: simplex transmit does not run the demodulator,
so nothing comes back):

    ./m17web -r sim -a none -w 8081
    python3 tools/wstest.py --port 8081 --cmd "sa G4GUO"
    python3 tools/wstest.py --port 8081 --cmd "td" --duration 18
    grep GNSS run.log

## Transmit samples used to be dropped: the rates and the fix

The transmit path is fed by frames, so its rates and its timing budget both
matter.  On the Pluto the whole chain is an exact integer division of the
AD9361's clock - there is no fractional resampling anywhere:

| stage | rate | ratio to the next |
|---|---|---|
| AD9361 sampling_frequency | 3 072 000 S/s | /8 in the AD9361 |
| Pluto stream (`cf-ad9361-lpc`) | 384 000 S/s | /48 |
| voice audio (codec2 3200) | 8 000 Hz | x6 (ALSA plug, card does 48 kHz) |
| M17 symbols | 4 800 sym/s | x80 = 384 000 (`radio_get_oversample()`) |
| modulator block `N_SAMPLES` | 1 920 samples | x8 = 15 360 = one 40 ms sub frame |

384000/8000 = 48, 48000/8000 = 6, 384000/48000 = 8, 4800 x 80 = 384000, and
1920 x 8 = 15360 samples = exactly one 40 ms M17 sub frame = 2 codec frames.
The ratio being integral means chunking can be aligned to whole audio frames and
cannot drift - it does not mean the two crystals are locked, which is why the
capture and transmit buffers still exist.

What actually went wrong was the **timing budget**.  `make` with `DIAG=1` prints
the transmit loop's per-sub-frame cost (`diag-tx:`), and it read:

```
audio(incl pace)=6us  encode=20228us  send+radio=26485us  total=47525us  (40000us = real time)
```

46.7 ms of work for 40 ms of stream is 17% slow, so the DAC starved (repeated
samples - the far end hears dropped transmit audio) and the microphone ring
overflowed (samples dropped before they were ever encoded).  The two costs:
codec2 3200 encoding is ~10 ms per 20 ms frame on a Pi 4B (half the budget on
its own), and pushing one 1920 sample block per `iio_buffer_push` cost ~26 ms
per sub frame in USB overhead - 3 ms a push, eight pushes a frame.

Fixes, all measured afterwards:

* `pluto_tx_samples()` accumulates and pushes **whole 40 ms sub frames**
  (`TX_CHUNK_SAMPLES` 15360) instead of eight 5 ms blocks: 26.5 ms -> 6.5 ms.
* `audio_alsa_input()` no longer reads the card from the transmit loop.  A
  thread drains the card continuously into a 2 s ring, so a slow moment in the
  loop cannot overflow the card's ring and silently discard microphone audio.
  Overruns and lost samples are counted now (`audio_tx_overruns()`,
  `audio_tx_dropped()`), because they used to be invisible.
* `audio_input()` paces on a **running deadline** rather than sleeping what is
  left of the current block, so the encode/modulate/push time is charged to the
  40 ms frame instead of being added on top of it.
* `m17_tx_lead_in()` sends a few **blank codec frames** (encoded silence, not
  zero bytes - the first payload logs as `C0006A439CE42108`) before the voice,
  so the receiver's acquisition and the microphone's start-up fall in silence
  instead of in speech.  `lead N` (or `M17WEB_TX_LEAD`) sets it, 2 frames = 40 ms,
  default 4.
* `audio_input_flush()` at key-down drops what the microphone captured while the
  radio was starting up (~5 s on this Pluto), which otherwise made the whole
  over lag the microphone by the depth of the buffer.

After that: transmit loop 39 990-40 010 us per 40 000 us frame, audio
consumption 8010 samples/s against the card's 7986, `tx_drops=0`,
`tx_overruns=0`, ring depth 0 (live audio, no lag).  The pending measurements
are `tx_drops` and `tx_overruns` in the state JSON, shown in the UI's Audio
panel next to the blank-frame setting.

## Hardware PTT on the Pi's header

A switch on the Raspberry Pi's 40 pin header can key the station, so a hand
microphone, a foot switch or a home made PTT box works without touching the
browser:

| Signal | BCM | Header pin | Behaviour |
|---|---|---|---|
| PTT input | GPIO 11 | pin 23 | a switch **to ground**; read with the internal pull-up, so pressed reads low |
| Keying output | GPIO 10 | pin 19 | driven **high** while transmitting (3.3 V - use a transistor for anything bigger) |

Pressing the switch transmits exactly as the button in the web interface does:
the transmitter indicator follows either source, because the state now reports
the transmitter as the radio sees it rather than as the last command left it.
Releasing returns to receive, but only if the hardware was what keyed the
station, so a release cannot cancel a transmission started from the browser.

Two things had to be fixed for this to work at all:

* **The pins are driven with libgpiod through `/dev/gpiochip0`** (`sudo apt
  install libgpiod-dev`), not the legacy `/sys/class/gpio` interface.  That
  interface is closed on current kernels - writing to `/sys/class/gpio/export`
  returns `EINVAL` even as root - which is why the pins could never be exported.
* **Nothing polled the switch.**  `radio_keyed()` knew how to read it (and
  upstream's version always called the Lime function, so a Pluto station could
  never see its own PTT line), but no thread ever called it.  One now reads it
  every 25 ms.

Without libgpiod the program still builds and runs, and the interface says
"hardware PTT not built in" rather than showing a pin state.  `charles` is in the
`gpio` group, so claiming the lines needs no root.  Note that GPIO 10 and 11 are
the SPI0 MOSI and SCLK pins: enabling an SPI device would take them away.

Both the claim and the keying can be checked from the hardware side:

```bash
gpioinfo | grep -A1 '"GPIO1[01]"'
#   line 10: "GPIO10"  output consumer="m17web"
#   line 11: "GPIO11"  input active-low bias=pull-up consumer="m17web"
pinctrl get 10        # while transmitting: "10: op -- pd | hi"
```

Without a switch to hand, a press can be simulated by moving the pin's internal
pull, which is the same register a real switch pulls against:

```bash
pinctrl set 11 ip pd    # reads low = pressed: the station transmits
pinctrl set 11 ip pu    # released again
```

The interface shows the switch state under the push to talk button
(`hardware PTT: GPIO 11 ready · keys GPIO 10`, red while pressed).

## The mixer: the card's own controls

A USB sound card has its own hardware controls, and they are not the application's
volumes.  A C-Media "USB Headphone Set" powers up at **29% (-20 dB)**, which is
quiet enough to lose received audio completely - and nothing else on the station
sets it, so the audio appeared to work while nothing was audible.

`mixer` lists the card's controls with their current values:

```
>> mixer
PCM=29 Mic=0(mic) Auto Gain Control=100
```

`mixer <number> <percent>` sets one, by the number from that list (the names come
from the card and may contain spaces, which a command line cannot carry):

```
>> mixer 0 100          # PCM to full
>> mixer 1 60           # the microphone gain (this one is capture)
>> mixer 2 off          # a switch, e.g. Auto Gain Control
```

The setting is written to `config.txt` like the other settings, so it is applied
again at the next start - and the program also re-applies it every time it opens
the audio, so a card that drops off USB and comes back gets its levels back with
it.  The **Mixer** panel in the web UI does the same thing with sliders.

A control that carries **both** a playback side and a capture side - a `Mic` with
a monitor level, which the USB PnP card has - is treated as the microphone's:
reading it shows the capture gain, and setting it moves the gain rather than the
monitor level.  Testing playback first reported 0% for that microphone and set
the wrong side.

This is what makes `amixer`, `alsactl store` and an `ExecStartPre=` line in the
unit unnecessary: the levels belong to the application that is using the card.

## Bluetooth audio: a speaker or a headset

A Bluetooth device is not a sound card, so it needs a backend that speaks Linux
audio.  This station deliberately has no PulseAudio or PipeWire, so the backend
is **BlueALSA** (`bluez-alsa-utils` and `libasound2-plugin-bluez`), which
registers each connected device with ALSA as a PCM the existing ALSA code can
open unchanged:

    bluealsa:DEV=F4:4E:FD:5D:F9:A1,PROFILE=a2dp    A2DP, playback only
    bluealsa:DEV=E8:07:BF:39:62:F9,PROFILE=sco     HFP, both directions

Two profiles matter, and one device does one of them at a time:

| mode | what it is | microphone |
|---|---|---|
| **speaker** | A2DP: the best audio, ~100-200 ms of latency | stays on the sound card |
| **headset** | HFP: both directions at 8 kHz mono, i.e. M17's own rate | the headset's own |

Nothing is lost by HFP's 8 kHz: M17 carries Codec2 at 3.2 kbps.

The console (and the **Bluetooth...** button in the Audio panel) drives it:

    bt                     what Bluetooth knows, and what is in use
    bt use MAC speaker     trust it, connect, and send the output to it
    bt use MAC headset     ...as a headset, microphone included
    bt scan                look for devices for 20 seconds
    bt off                 back to the Pi sound card

`bt use` writes itself into `config.txt` (as `btdev MAC ROLE`), so the choice
survives a restart.  Details worth knowing:

* **A device has to be trusted, not just paired.**  BlueZ refuses the profile
  connection otherwise, which reads as a connection attempt that is cancelled
  with no explanation.  `bt use` trusts as part of connecting.
* **The audio falls back to the sound card** if the device is off, out of range,
  or not connected yet, rather than going silent - the log says which.
* Pointing the audio at a device connects it **first**: bluealsa only creates
  the A2DP PCM once the profile is up, so routing to a PCM that does not exist
  yet is a race.
* With no bluealsa installed nothing changes: `bt` says why, and the station
  behaves exactly as before.

## Modes, and changing them while running

The mode is how the station is wired up:

| Mode | Channel type | What it does |
|------|--------------|--------------|
| `radio` | DRTOAS | RF in, local audio out - the normal voice mode |
| `loop` | ASTOAS | the audio is looped back inside the program, for practice with no radio |
| `gate` | DRTODN | the radio is bridged to a reflector |
| `client` | ASTODN | local audio is bridged to a reflector |

Pick one in the **Mode** panel, or type `mode radio` (or `loop`, `gate`, `client`).
The mode used to be settable only at startup: the dispatcher in `m17_tx_rx.cpp`
calls the thread for the current mode *synchronously*, and those threads loop
until the program stops, so they never returned and the channel type was never
re-read - which is what the old "only valid at startup" comment described.  Each
mode thread now remembers the mode generation it started for and checks it through
`M17_MODE_OK`, so a change makes the running thread step aside cleanly, the
dispatcher starts the new one, and the change takes effect within a frame or two
with no restart.  A mode change also returns to receive, so a transmission cannot
be carried across it, and leaving a network mode disconnects from the reflector
politely first.

The choice is written into `config.txt` (its `mode` line is replaced, via a
temporary file and a rename, so a failure cannot leave a half-written config), so
it survives a restart.  The state reports the live channel type rather than a
saved string, which is what the panel's radio buttons highlight.

Verified on the Pi: `loop` -> `radio` -> `gate` -> `client` -> `radio`, each
taking effect while running with the same process throughout (no restart, no
crash); an unknown mode answered with the list of valid ones; a mode change during
a transmission returning to receive; and a transmit in radio mode afterwards still
clean (`tx_drops=0`, `tx_overruns=0`).

## Connecting to a reflector

The Reflector panel takes a name, a module letter and a connect button; the same
two commands work in the console (`conn M17-M17 C`, `disc`).  Three separate
faults stopped this working, and all three had to go:

* **The name could not be typed.**  The field was a `<select>` filled from
  `M17Hosts.txt`, so only the entries in that file could be chosen - and it lists
  bare designators (`M17`) while every published reflector name is the full form
  (`M17-M17`).  It is now an `<input>` with a `<datalist>`: type anything, or pick
  from the list, which is labelled with the full names.  Lookup matches on the
  designator with or without the leading `M17-`, so *M17*, *M17-M17* and whatever
  the published list uses all resolve to the same reflector.
* **Nothing was ever sent.**  `m17_net_open()` - which creates the UDP sockets -
  was only called by the `gate` and `client` modes, so in the normal `radio` mode
  pressing connect did literally nothing: no socket, no CONN packet, and the
  status stayed "not connected" for ever.  The sockets are now opened on demand
  by `conn`, and opening twice is harmless.
* **The address was years out of date.**  `M17Hosts.txt` was written in June 2022
  and still pointed M17-M17 at an address it left long ago, which looks exactly
  like a broken connect button.  Refresh it from the M17 Project's published
  list:

```bash
python3 tools/update_hosts.py --also /home/charles/m17web
```

The tool fetches `https://m17-project.github.io/hostfiles/M17Hosts.json` (the
current list - 133 reflectors when this was written, generated by W0CHP from
RefCheck.Radio data), writes the `Designator IP Port` file the program expects
with full designators, and can copy it to the runtime directory the service uses.

### Testing your own audio: the reflector's parrot

Every mrefd reflector can echo your own stream straight back, which tests the
whole transmit path without a second operator.  Set the destination to `PARROT`
and transmit for a few seconds:

```
da PARROT        # in the console, or type PARROT in the interface's destination box
```

Then key up: you should hear your own voice back about 40 ms after you stop.  The
destination field must say `PARROT` for the test (the interface shows it as soon
as you set it), and remember to set it back to `ALL` (or press **broadcast (ba)**)
afterwards.

mrefd's documentation says to use `#PARROT`, and that cannot work here: `#` is not
one of the 40 M17 characters (space, A-Z, 0-9, `-`, `/`, `.`), so it cannot be
encoded into an address.  mrefd also triggers the parrot on any destination
*containing* the word PARROT, which is what this uses.

This is the check that proves a quiet transmitter: capture the packets and every
returned voice payload is byte-identical to what the station sent, e.g.

```
frames returned after key-up: 205
returned stream source: 0000025ea29f (unchanged - mrefd leaves the source alone)
payloads matching what we sent: 205 of 205
```

If that works and nobody can hear you, the problem is at the far end (a listening
app on the wrong module, or a browser that has not been allowed to play audio),
not in the station.

### Client or gateway: the callsign suffix matters

Only a *gateway* appends the letter `G` to its callsign when it connects.  The
code used to add it unconditionally, which is right for `gate` mode (a gateway
carrying traffic from the radio) but wrong for `client` mode: the audio frames
carry the plain callsign in their LSF, so the reflector saw stream traffic
arriving "from G4GUO" over a link registered as "G4GUO G" and dropped it.

That failure is invisible from the station's side, which is what makes it worth
knowing: the link reads connected, the reflector answers its PING, the meters
move, and packets leave the Pi in correct M17 framing with real audio in them.
Capture and decode them and everything looks perfect - the only evidence is at the
far end.  The suffix is now added in `gate` mode only.

The reflector's own dashboard is the way to check this end to end.  With the fix,
M17-M17's "Last Heard" shows:

```
England | G4GUO | IO90st | @ALL | Stream | G4GUO | 26 seconds ago | C
```

Note the locator: the dashboard parses the GNSS position out of the META field, so
that line also confirms the GPS to META chain.

Failures are no longer silent: an unknown name is reported ("reflector 'X' is not
in M17Hosts.txt"), and a connect logs the address it is using
(`connecting to M17-M17 module C at 107.191.121.105:17000`).  The state carries
`ref.connected`, which is what the badge in the interface shows.

Verified against the live M17-M17 reflector: connect on module C reaches
`107.191.121.105:17000` and the state reports connected; disconnect returns it to
not connected.  Remember that a reflector carries your callsign and audio to
everyone linked to it, and disconnect when you have finished.

## Running it as a service, and surviving the radio disappearing

The Pluto drops off USB from time to time (it re-enumerated at 20:29 on 13 Oct
2026) and anything that does not survive that stays dead until somebody notices.
Run m17web under systemd so it comes back on its own:

```bash
cd /mnt/hf/shared/projects/m17_sdr_web
sudo cp m17gismo/tools/m17web.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now m17web

systemctl status m17web           # is it up
sudo journalctl -u m17web -f      # what it is doing (this is its log now)
sudo systemctl restart m17web     # restart it by hand
```

Two things make the recovery work, and both were missing before:

* **The program exits deliberately when the radio has gone.** `pluto_rx_samples()`
  counts consecutive failed reads and `exit(1)`s after about two seconds of them,
  with a line in the journal saying why. Without that it would keep running,
  deaf and mute, with a web interface that still answered - which is exactly
  what "the program has stopped" looked like from the outside.
* **The iio calls cannot hang or crash the program.** A failed
  `iio_buffer_refill()` returns a *negative* value; upstream stored it in an
  unsigned int, so -1 became ~4 GB and the following `memcpy` **segfaulted**,
  taking the web UI down with it. The context also had an infinite timeout, so a
  dead link blocked the sample loop forever. Both are fixed: the return value is
  checked and clamped, and the timeout is 2 s.

`tools/pi_m17web.sh` still works for a one-off foreground-style run, and now
says so instead of fighting the service for port 8080 when the service is
running.

### Where its settings live

The program loads `config.txt` from its **working directory**, so the service
runs with `WorkingDirectory=/home/charles/m17web` and keeps its own `config.txt`
there (`mode radio`, `sa G4GUO`, the frequencies).  The build directory keeps
upstream's sample config, which has `mode gate` in it - a reflector gateway,
which ignores the local microphone and speaker completely.  Running the service
from the build directory therefore produced a station that looked alive, decoded
frames, and had no voice path at all.

### PulseAudio is stopped and masked on this Pi

The recommended audio path talks to the card directly, and the daemon only ever
competed for it: devices opening with "Device or resource busy" at start-up, and
the microphone failing to open at key-down.  Both `pulseaudio` and `wireplumber`
are disabled and masked in the user scope, so nothing restarts them.

To get the `audio pulse` option back (see the audio section above):

```bash
systemctl --user unmask pulseaudio.socket pulseaudio.service wireplumber.service
systemctl --user enable --now pulseaudio.service
```

### Checking it is actually working

The state in the web UI carries what you need to tell "silent" from "broken":

| Field | Meaning |
|---|---|
| `spk_open`, `mic_open` | which audio streams are really open (the microphone is closed while receiving, by design) |
| `alsa.error` | only set while the stream it refers to is *closed* - a stale message used to sit there for hours |
| `tx_drops` | samples of your voice lost while transmitting; 0 is clean, and it resets at each key-down |
| `tx_overruns` | sound card capture overruns, the usual cause of the first number not being 0 |

## Audio: which device the voice comes from and goes to

The web UI has an **audio device** selector (the "Audio" panel) plus the
equivalent commands, and the choice is applied live - the radio loop re-opens
its streams within one transmit/receive cycle, no restart needed:

| UI option | command | what it does |
|---|---|---|
| Raspberry Pi sound card (direct) | `audio alsa [device]` | the card through **ALSA**, no daemon in the path |
| browser microphone / speaker | `audio browser` | WebAudio in the page, 8 kHz over the WebSocket |
| Raspberry Pi sound card (PulseAudio) | `audio pulse` | the same card through PulseAudio |
| browser + Pi sound card | `audio both` | mic from the browser (falling back to the card), audio out to both |
| mute | `audio none` | silence in, discard out |
| (testing) | `-a file:in.raw:out.raw` | raw 8 kHz files instead of a device |

`make` builds the ALSA back end when `libasound2-dev` is present (`ALSA=1`,
override with `ALSA=0`) and the PulseAudio one when `libpulse-dev` is
(`PULSE=1`).

**Use the direct ALSA option on a Pi.** Two reasons, both measured on this one:

* Debian routes ALSA's `default` PCM *through* PulseAudio, so "plain ALSA" is
  still PulseAudio unless you name a real device.
* With PulseAudio installed, the app's streams attached to the USB card
  (`pactl` showed `sink-input ... s16le 1ch 8000Hz`, `Corked: no`) while the
  sink stayed `SUSPENDED` with the ALSA device `closed` and a 190 s buffer, i.e.
  PulseAudio accepted writes but never ran the card - even for a direct
  `pacat`.  `aplay -D plughw:3,0` worked, which is what pointed at ALSA.

The direct back end picks its device itself: the configured device first
(`audio alsa plughw:3,0`, or `M17WEB_ALSA_DEVICE`), otherwise the first USB/PnP
card it finds as `plughw:<card>,0` (rate conversion included, so 8 kHz mono
just works), otherwise `sysdefault`.

Verified on the Pi: with `-a alsa` the state reports
`audio=alsa alsa={'compiled': True, 'ready': True, 'device': 'plughw:3,0'}`,
the card's ALSA capture PCM shows `state: RUNNING owner_pid <m17web>`, CPU stays
around 4%, and switching `alsa -> browser -> alsa` in the UI is picked up live.
The playback PCM occasionally reports `XRUN` in `mode loop` (audio loopback is
the one mode that reads and writes the same card at once, so the writer falls a
fraction of a period behind); the back end re-prepares and continues, and the
real transmit/receive modes do not read and write simultaneously.

## Simulated radio (no hardware needed)

`-r sim` loops transmit samples back into the receiver through the same 8x
decimating filter the Pluto path uses, adds adjustable noise and paces the
receive path to real time.  It exercises the whole chain - modulation, sync,
Golay/LICH decode, framing, Codec2 - with `gerr=0` on a clean channel, so the
web UI, audio path and protocol code can be developed without an SDR attached.

Commands: `sim loop on|off`, `sim realtime on|off`, `sim noise 0.0-0.5`.

## Testing tools (`tools/`)

| Tool | Purpose |
|------|---------|
| `wstest.py` | headless WebSocket client (stdlib only): commands, hold PTT, stream a `.raw` file into the mic, save decoded audio |
| `audio_verify.py` | decoded audio against a reference `c2enc`/`c2dec` decode: envelope correlation (Codec2 is a vocoder, sample correlation is not meaningful) |
| `audio_compare.py` | chunked, drift tolerant comparison of two raw files |
| `pi_m17web.sh` | start/stop/status/log on the Pi with a generated test `config.txt` |
| `deploy_pi.sh` | rsync the sources to the Pi and build there |
| `embed_www.py` | generate `www_assets.h` from `www/` |
| `gps_meta_decode.py` | decode an M17 META GNSS payload, written from the specification (not from the C) so the encoder can be checked independently |

Example session (on the Pi):

    cd m17gismo && make sim
    MODE=radio bash tools/pi_m17web.sh start
    python3 tools/wstest.py --cmd "sa G4GUO" --cmd "td" --ptt \
        --wav-in ~/codec2/raw/hts1a.raw --wav-out /tmp/dec.raw --duration 6
    python3 tools/audio_verify.py ~/codec2/raw/hts1a.raw /tmp/dec.raw

## Status

Verified on a Raspberry Pi 4 (Debian 13, Codec2 built from source):

* builds clean as `make sim` (web UI + simulator, no SDR libraries)
* HTTP serves the app; WebSocket handshake, state, commands, help, reflector
  list and log replay all work headlessly (`tools/wstest.py`)
* audio flows both ways over the WebSocket (mic frames in, decoded frames out)
  and through the file back end
* duplex through the simulator decodes its own transmission with `gerr=0`
* clean SIGTERM/SIGINT shutdown, including from a browser `q` command
* both codec back ends work in the app (mode loop and through the modem)
* the audio device selector works live for browser, direct-ALSA and PulseAudio
  back ends, including the Pi's USB sound card (see the audio section above)
* the transmit path runs in real time on the Pluto with the Pi's sound card:
  no dropped samples, no card overruns, blank codec frames before the voice
  (see the transmit section above)

Also deployed against real hardware: a PlutoSDR (ADALM-PLUTO, fw v0.33,
reachable at 192.168.2.1 over the USB RNDIS interface ```eth1```).  The web UI
runs on the Pi with ```-r pluto``` and ```iio_attr``` confirms the radio is being
driven: RX and TX LO both 146 400 000 Hz, RX sample rate 384 kS/s (3.072 MS/s
with the x8 decimation the code expects), rf_bandwidth 200 kHz, live RSSI in
the UI.  PulseAudio and the simulator are still compiled in, so
```-r sim```/```-a pulse``` remain available as fallbacks.

Not done yet, in priority order:

1. **Voice fidelity** - a clean simulator should reproduce the reference Codec2
   decode almost exactly; decoded audio currently correlates about 0.45
   (envelope) with the original speech after ~1.1 s of pipeline latency.  That
   points at a lost or repeated subframe in the M17 stream path (or my test
   harness's pacing); compare sent vs decoded payloads frame by frame.
2. **Hardware PTT** - ```rpi_gpio.cpp``` uses the legacy sysfs GPIO interface,
   which this kernel rejects outright (```echo 10 > /sys/class/gpio/export``` gives
   EINVAL even as root), so the physical PTT input/output pins do not work;
   PTT from the browser does.  Fixing it means rewriting ```rpi_gpio.cpp``` on
   libgpiod.  (The export/unexport swap found upstream was still wrong and is
   fixed.)
3. **Partly persistent** - the Pluto interface now has a NetworkManager profile
   (```nmcli con show pluto```: ```eth1```, static ```192.168.2.2/24```,
   autoconnect), which was needed because the Pluto re-enumerates on USB and the
   hand-added address disappeared with it.  Nothing yet starts m17web at boot;
   a systemd unit is the remaining step.  The program no longer dies when the
   Pluto is missing - it reports why and falls back to the simulator so the web
   UI stays usable.
4. **LimeSDR Mini** - now the station's radio, and the path is live: the board is
   opened over LimeSuite (`LimeSDR Mini ... serial=1D394100534EFA`, 40 MHz
   reference, `LNAW` on receive, `Band 2` on transmit, both chains calibrated,
   48 kS/s = 10x the 4800 sym/s symbol rate to match `radio_get_oversample()`),
   and the hardware PTT reads the **Pi's** GPIO 10/11 - `radio_keyed()` routes the
   LIME case to `rpi_read_ptt()` exactly as PLUTO does, because only the
   LimeSDR-USB has a GPIO header and a failed `LMS_GPIORead` on a Mini reads back
   as permanently keyed.  On-air transmit and the parrot test are still to be
   done (the antenna has to move to the Mini's SMA).  See the
   `limesdr-limesuite-setup` skill for the LimeSuite-on-Debian trap: the `+dfsg`
   package ships no gateware, so `LimeUtil --update` must run once or the board
   looks dead.
5. The web UI has been driven headlessly (curl + `wstest.py`) but not clicked
   through in a real browser.
6. Reflector gateway mode (`mode gate`) has not been tested against a live
   reflector.
7. Upstream has no licence file; this fork inherits that (all rights reserved by
   default) - worth clarifying with the author before publishing.

## Licence

The code here is released under the **BSD Zero Clause License (0BSD)** - see
[LICENSE](LICENSE).  It is the most permissive licence available: use, change and
redistribute it for any purpose, with no conditions and no requirement to credit
anyone.  Where that is too lax for your taste, MIT (same terms plus attribution)
or GPL-3.0 are one-line swaps.

Two components vendored in the tree keep their own licences and are **not**
covered by it: the modified Codec2 in `m17gismo/codec2mod/` (LGPL-2.1) and the
LimeSuite headers (Apache-2.0).  See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)
for what that means in practice.