#!/usr/bin/env bash
# sync.sh - copy the FARGAN runtime from an Opus source tree into this folder
#
#   bash third_party/opus/sync.sh /path/to/opus
#
# Only the files FARGAN needs are copied. A few include lines are rewritten so the
# code builds without the rest of Opus (no generated PLC, DRED or pitch models).
# Every rewrite is listed below and in README.md. The Opus licence is in COPYING.
set -euo pipefail
SRC=${1:?usage: sync.sh /path/to/opus}
DST=$(cd "$(dirname "$0")" && pwd)

files="
COPYING
include/opus_types.h
include/opus_defines.h
celt/arch.h
celt/os_support.h
celt/cpu_support.h
celt/x86/x86cpu.h
celt/x86/x86_arch_macros.h
dnn/fargan.c
dnn/fargan.h
dnn/nnet.c
dnn/nnet.h
dnn/nnet_arch.h
dnn/nnet_default.c
dnn/parse_lpcnet_weights.c
dnn/vec.h
dnn/vec_avx.h
dnn/vec_neon.h
"
for f in $files; do
  mkdir -p "$DST/$(dirname "$f")"
  cp "$SRC/$f" "$DST/$f"
done

# fargan.c: drop LPCNet/PLC headers, FARGAN needs none of them
sed -i -e '/#include "freq.h"/d' -e '/#include "lpcnet.h"/d' -e '/#include "pitch.h"/d' \
       -e 's/#include "lpcnet_private.h"/#include "fargan_defs.h"/' "$DST/dnn/fargan.c"
# fargan.h: constants come from fargan_defs.h instead of freq.h and pitchdnn.h
sed -i -e 's/#include "freq.h"/#include "fargan_defs.h"/' -e '/#include "pitchdnn.h"/d' "$DST/dnn/fargan.h"
# nnet.c: no DRED or PLC models are built, their size constants come from fargan_defs.h
sed -i -e 's/#include "dred_rdovae_constants.h"/#include "fargan_defs.h"/' -e '/#include "plc_data.h"/d' "$DST/dnn/nnet.c"

( cd "$SRC" && git log -1 --format='%H %cd' 2>/dev/null ) > "$DST/UPSTREAM_COMMIT" || echo unknown > "$DST/UPSTREAM_COMMIT"
echo "synced from $SRC ($(cat "$DST/UPSTREAM_COMMIT"))"
