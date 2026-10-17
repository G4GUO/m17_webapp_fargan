# Third-party components

The licence in [LICENSE](LICENSE) covers this project's own code. It does not
cover the components below, which keep their own licences.

## Vendored in this repository

| Where | Component | Licence |
|---|---|---|
| `m17gismo/codec2mod/` | [M17-Project/Codec2-mod](https://github.com/M17-Project/Codec2-mod) @ `7c334283` (2026-09-30) | LGPL-2.1 - full text in `m17gismo/codec2mod/LICENSE` |
| `m17gismo/c2fargan/` | [M17-Project/Codec2-mod-neural](https://github.com/M17-Project/Codec2-mod-neural) @ `1c32682` (2026-10-17) - the FARGAN neural receiver, by **Wojciech Kaczmarski SP5WWP** | LGPL-2.1 - full text in `m17gismo/c2fargan/LICENSE` |
| `m17gismo/c2fargan/third_party/opus/` | FARGAN runtime from [Opus](https://opus-codec.org), Xiph.Org Foundation | BSD-3-Clause - see `m17gismo/c2fargan/third_party/opus/COPYING` |
| `m17gismo/LimeSuite.h`, `m17gismo/LMS7002M_parameters.h` | LimeSuite, (C) 2016 Lime Microsystems | Apache-2.0 |

`m17gismo/c2fargan/` is the neural receiver, and it is **Wojciech Kaczmarski
SP5WWP**'s work: an M17 Project component that decodes the same Codec2 3200
frames with FARGAN instead of the sinusoidal model.  It is built in with
`NEURAL=1` (the default) and left out with `NEURAL=0`; the receiver is
receive-only, so it changes nothing another station receives.  FARGAN itself is
from Opus, and Codec2 itself is David Rowe VK5DGR's.  Provenance and commit:
`m17gismo/c2fargan/VENDORED.txt`.

The Codec2-mod sources are compiled into the binary with the rest of the
program (`make CODEC2_BACKEND=mod`), so distributing a binary means distributing
LGPL-2.1 code with it.  That is satisfied by this repository shipping the
component's source and its licence: anyone receiving a binary can rebuild it
with a modified Codec2-mod, which is what LGPL-2.1 asks for.  Keep
`m17gismo/codec2mod/` and its LICENSE with any copy you pass on.

## Linked at build time (not vendored here)

The program links against these libraries, which are LGPL-2.1 licensed:

- libiio - the IIO interface used to talk to the PlutoSDR
- ALSA (`libasound`) - the direct sound card back end
- PulseAudio (`libpulse-simple`) - the PulseAudio sound card back end
- Codec2 (`libcodec2`) - when built with `CODEC2_BACKEND=lib`

See each project for its exact terms.  Linking against them does not change the
licence of this project's own code.

## The M17 protocol

M17 is an open standard from the [M17 Project](https://m17project.org).  This
program implements it; the protocol itself is not covered by the licence here.
