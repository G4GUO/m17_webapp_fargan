# m17web — M17 digital voice over SDR, with a web interface

Receive and transmit **M17** digital voice with a PlutoSDR, a LimeSDR, or no
radio at all, and control the whole station from a web browser on any machine on
your network.  This is a fork of [G4GUO/m17_sdr](https://github.com/G4GUO/m17_sdr)
with the terminal interface replaced by a browser one; the M17 protocol, DSP and
networking are upstream's.

    Host (Windows / Linux)                     Raspberry Pi 4 + SDR
    +------------------------+   WebSocket   +---------------------------+
    | browser UI             |<------------->| m17web                    |
    |  . PTT + meters        |  JSON state   |  . HTTP + WS server       |
    |  . frequencies/gains   |  PCM audio    |  . M17 modem (unchanged)  |
    |  . reflector control   |               |  . Pluto / Lime / SIM     |
    |  . console + log       |               |  . Codec2 3200 voice      |
    +------------------------+               +---------------------------+

## What you get

* **A browser station.** Push-to-talk, RX/TX frequencies and gains, signal and
  power meters, reflector selection, a live log and every console command — all
  on one page, sized to fit a screen.
* **Audio where you want it.** The browser's own microphone and speaker, the
  Pi's sound card, a Bluetooth speaker and headset, or silence — chosen
  separately for the speaker and the microphone in one Audio configuration box,
  with the gain and volume applied on the Pi so the meters show what is really
  going out.
* **No hardware needed to try it.** The simulated radio runs the whole UI, the
  voice path and the logging on any Linux box.
* **Optional extras:** the FARGAN neural receiver (16 kHz voice from the same
  3200 bps bitstream), GPS position in the M17 link setup frame, recording of
  what the receiver decodes, and hardware PTT on the Pi's GPIO header.

## Requirements

* Linux with gcc/g++, make.  Built and tested on Raspberry Pi OS (Pi 4B).
* [Codec2](https://github.com/drowe67/codec2) installed from source (the build
  needs its headers; `CODEC2_BACKEND=mod` still uses them).
* Optional, for real radios and local audio: `libiio-dev` (Pluto),
  `liblimesuite-dev` (LimeSDR), `libasound2-dev` (sound card),
  `libpulse-dev` (PulseAudio), `libgpiod-dev` (hardware PTT).
* Optional, for Bluetooth audio: `bluez-alsa-utils` and
  `libasound2-plugin-bluez`.

## Build

```sh
sudo apt install cmake libasound2-dev libiio-dev limesuite liblimesuite-dev libgpiod-dev

# Codec2 from source
git clone https://github.com/drowe67/codec2.git && cd codec2
mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j4
sudo make install && sudo ldconfig
cd ../..

git clone https://github.com/G4GUO/m17_webapp_fargan.git
cd m17_webapp_fargan/m17gismo
make LIME=1 SIM=1 CODEC2_BACKEND=mod
```

That produces `m17web`.  The flag set is not optional on Debian: a bare `make`
leaves out the simulated radio and the vendored codec (which the sources
reference unconditionally, so the link fails), and it cannot see Debian's Lime
headers.  `make sim` builds a browser UI plus simulator with no radio and no
sound card, which is the quickest way to see what the program does.

## Run

```sh
./m17web -r sim   -w 8080        # simulated radio - no hardware
./m17web -r pluto -w 8080        # PlutoSDR / Pluto+
./m17web -r lime  -w 8080        # LimeSDR
```

Then open `http://<pi-address>:8080/`.  Settings live in `config.txt` in the
working directory (callsign, mode, frequencies, audio devices, Bluetooth
choices) and are read at startup; everything can also be changed while running,
from the page or the console.

| option | meaning |
|---|---|
| `-r sim\|pluto\|lime` | radio backend |
| `-w PORT` | web UI port (default 8080) |
| `-p IP` | Pluto address (default `pluto.local`) |
| `-a browser\|pulse\|both\|none\|file:IN:OUT` | audio backend (the whole pair; the interface sets the two routes instead) |

Browser audio needs one click to start (browser autoplay rules), and a browser
will only give a web page the microphone on a secure origin — `localhost` or
HTTPS.  The Pi's sound card or a Bluetooth headset avoid the problem entirely
and are what most stations use.

## Install as a service

```ini
# /etc/systemd/system/m17web.service
[Unit]
Description=m17web - M17 digital voice over SDR (web interface)
After=network-online.target sound.target bluetooth.target

[Service]
WorkingDirectory=/home/pi/m17web
ExecStart=/home/pi/m17web/m17web -r pluto -w 8080
Restart=always

[Install]
WantedBy=multi-user.target
```

```sh
sudo systemctl daemon-reload && sudo systemctl enable --now m17web
```

Run it from a directory of its own, with its `config.txt`: the program rewrites
that file with the settings you change from the page.

## Documentation

* **[User manual (PDF)](docs/m17web-user-manual.pdf)** — written for the
  operator: setting up, making a contact, the audio options, every control and
  command, troubleshooting.  Generated from `docs/user_manual.py`.
* **[docs/TECHNICAL.md](docs/TECHNICAL.md)** — design notes, measurements and
  the history: why the audio back end is ordered the way it is, the transmit
  timing budget, the Bluetooth traps, the GPS payload layout, the Codec2
  comparison, and what changed relative to upstream.

## Licence and credits

0BSD.

* M17 implementation: **G4GUO** ([m17_sdr](https://github.com/G4GUO/m17_sdr)).
* FARGAN neural decoder: **Wojciech Kaczmarski SP5WWP**, M17 Project
  ([Codec2-mod-neural](https://github.com/M17-Project/Codec2-mod-neural)).
* Codec2: **David Rowe VK5DGR** and contributors.
