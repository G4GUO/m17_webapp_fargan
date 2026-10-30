# m17web makefile
#
# Build matrices:
#   make                 web UI + Pluto + Lime + PulseAudio (needs all the libs)
#   make sim             web UI + simulated radio, no SDR libraries needed
#   make sim PULSE=0     web UI + simulated radio, no PulseAudio either
#   make console         original ncurses UI (legacy fallback)
#
# Options (override on the command line):
#   PULSE=0        no PulseAudio back end
#   PLUTO=0        no Pluto / libiio back end
#   LIME=0         no LimeSDR back end
#   SIM=1          include the simulated radio
#   CONSOLE=1      build the ncurses UI instead of the web UI
#   WEB=0          leave the web UI out
#   EMBED=1        embed www/ into the binary (default: 1 if www/ exists)
#   DEBUG=1        -O0 -g
#   CODEC2_BACKEND=lib|mod
#                  lib (default) = external libcodec2 (any mode)
#                  mod = the vendored modified Codec2 in codec2mod/ (M17-Project/
#                        Codec2-mod): 3200 bps only, which is what M17 uses, and
#                        no external Codec2 is needed at all
#
CC   = gcc
CXX  = g++
TARGET = m17web

# Without this, a bare "make" sometimes picks no goal at all and reports success
# without building anything.
.DEFAULT_GOAL := all

CODEC2_PREFIX ?= /usr/local
CODEC2_BACKEND ?= lib
PULSE  ?= $(shell pkg-config --exists libpulse-simple 2>/dev/null && echo 1 || echo 0)
PLUTO  ?= $(shell test -f /usr/include/iio.h -o -f /usr/local/include/iio.h && echo 1 || echo 0)
LIME   ?= $(shell test -f /usr/include/limesuite/LimeSuite.h -o -f /usr/local/include/LimeSuite.h -o -f /usr/include/LimeSuite.h && echo 1 || echo 0)
SIM    ?= 0
CONSOLE ?= 0
WEB    ?= 1
EMBED  ?= $(shell test -d www && echo 1 || echo 0)

CFLAGS   = -Wall -Wextra -O3 -I$(CODEC2_PREFIX)/include -I$(CODEC2_PREFIX)/include/codec2
CXXFLAGS = $(CFLAGS) -std=c++11
LIBS     = -lcodec2 -lpthread -lm -lrt -lstdc++

ifeq ($(DEBUG),1)
CFLAGS  += -O0 -g
endif

# DIAG=1 enables the receive path tracing in m17_rx_parse.cpp
ifeq ($(DIAG),1)
CFLAGS   += -DM17WEB_DIAG
CXXFLAGS += -DM17WEB_DIAG
endif

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
SRCS = $(wildcard *.cpp)

# Radio back ends
ifeq ($(PLUTO),1)
CFLAGS   += -DHAVE_PLUTO
CXXFLAGS += -DHAVE_PLUTO
LIBS     += -liio
else
SRCS := $(filter-out pluto.cpp,$(SRCS))
endif

ifeq ($(LIME),1)
CFLAGS   += -DHAVE_LIME
CXXFLAGS += -DHAVE_LIME
LIBS     += -lLimeSuite
else
SRCS := $(filter-out lime.cpp,$(SRCS))
endif

ifeq ($(SIM),1)
CFLAGS   += -DHAVE_SIM
CXXFLAGS += -DHAVE_SIM
else
SRCS := $(filter-out radio_sim.cpp,$(SRCS))
endif

# Audio back ends: ALSA talks to the sound card directly (no PulseAudio in the
# path), which is the reliable option on a Pi; PULSE goes through PulseAudio.
ALSA ?= $(shell test -f /usr/include/alsa/asoundlib.h && echo 1 || echo 0)

ifeq ($(ALSA),1)
CFLAGS   += -DHAVE_ALSA
CXXFLAGS += -DHAVE_ALSA
LIBS     += -lasound
else
SRCS := $(filter-out audio_alsa.cpp,$(SRCS))
endif

ifeq ($(PULSE),1)
CFLAGS   += -DHAVE_PULSE
CXXFLAGS += -DHAVE_PULSE
# -lpulse as well as -lpulse-simple: pa_strerror() lives in the former
LIBS     += -lpulse-simple -lpulse
else
SRCS := $(filter-out audio_pulse.cpp,$(SRCS))
endif

# Web UI needs the C server; the console build needs ncurses instead.
# The AX.25 packet code is taken verbatim from G4GUO/m17-ax25 and built as C.
CSRCS = kiss.c m17_packet.c m17_bridge.c
ifeq ($(WEB),1)
CXXFLAGS += -DUI_WEB
CSRCS    += webserver.c
else
SRCS := $(filter-out webui.cpp,$(SRCS))
endif

ifeq ($(CONSOLE),1)
CXXFLAGS += -DUI_CONSOLE
LIBS     += -lncurses
else ifeq ($(WEB),0)
$(error select a UI: CONSOLE=1 or WEB=1)
else
SRCS := $(filter-out ui_console.cpp,$(SRCS))
endif

# GPIO PTT (Raspberry Pi).  Harmless elsewhere: the pins are only touched when
# a real radio back end is in use.  The legacy /sys/class/gpio interface is
# closed on current kernels (export returns EINVAL even as root), so this needs
# libgpiod's character device: without the header the code still builds, but
# reports that hardware PTT is unavailable.
GPIOD ?= $(shell test -f /usr/include/gpiod.h && echo 1 || echo 0)
CFLAGS   += -DHAVE_RPI_GPIO
CXXFLAGS += -DHAVE_RPI_GPIO

# A stamp for the web page: a browser tab left open across a rebuild keeps running
# the old script, which then disagrees with the server (see the hello message in
# webui.cpp).  Only changes when the code does.
STAMP ?= $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
CFLAGS   += -DM17WEB_STAMP='"$(STAMP)"'
CXXFLAGS += -DM17WEB_STAMP='"$(STAMP)"' 
ifeq ($(GPIOD),1)
CFLAGS   += -DHAVE_GPIOD
CXXFLAGS += -DHAVE_GPIOD
LIBS     += -lgpiod
endif

# ---------------------------------------------------------------------------
# Codec2 back end
# ---------------------------------------------------------------------------
# The modified Codec2 (M17-Project/Codec2-mod) is vendored in codec2mod/ and has
# no build system of its own, so its sources are compiled here.  Its public
# symbols are renamed so that one binary can link both it and the stock
# libcodec2 - which is how the bitexact target compares the two.
CODEC2_MOD_RENAME = -Dcodec2_init=codec2raw_init \
                    -Dcodec2_encoder_init=codec2raw_encoder_init \
                    -Dcodec2_decoder_init=codec2raw_decoder_init \
                    -Dcodec2_encode=codec2raw_encode \
                    -Dcodec2_decode=codec2raw_decode
CODEC2_MOD_SRCS   = $(wildcard codec2mod/src/*.c) codec2mod/codec2_shim.c
CODEC2_MOD_OBJS   = $(CODEC2_MOD_SRCS:.c=.o)
CODEC2_MOD_CFLAGS = -Wall -Wextra -O3 -Icodec2mod -Icodec2mod/inc \
                    -DCODEC2_MOD $(CODEC2_MOD_RENAME)

ifeq ($(CODEC2_BACKEND),mod)
CFLAGS    += -DCODEC2_MOD -Icodec2mod
CXXFLAGS  += -DCODEC2_MOD -Icodec2mod
LIBS      := $(filter-out -lcodec2,$(LIBS))
EXTRA_OBJS = $(CODEC2_MOD_OBJS)
endif

# ---------------------------------------------------------------------------
# FARGAN neural receiver (M17-Project/Codec2-mod-neural)
#
# The decoder is Wojciech Kaczmarski SP5WWP's M17 Project component; FARGAN
# itself is from Opus (Xiph.Org Foundation, BSD-3-Clause).
# ---------------------------------------------------------------------------
# Receive-only: the same 8-byte Codec2 3200 frames the station already carries
# are synthesised by FARGAN at 16 kHz instead of by the sinusoidal model at
# 8 kHz, and decimated back to the audio path's 8 kHz.  The transmitter, the
# bitstream and every other M17 station are untouched, so it is a per-station
# option (config.txt "fargan on", or the checkbox in the Audio panel).
#
# It decodes the frames through the modified Codec2 it is vendored with, hence
# the CODEC2_BACKEND=mod requirement: with stock libcodec2 linked in, its own
# internal symbols (unpack, decode_Wo, ...) would answer these calls instead.
#
# Measured on the station's Pi 4B: 9.6 ms per 20 ms frame, 48% of one core,
# 2.1x real time, worst frame 10.4 ms of a 20 ms budget.  Numbers and harness:
# /mnt/hf/shared/projects/m17_neural/RESULTS.md
NEURAL    ?= 1
C2F_DIR    = c2fargan
C2F_OPUS   = $(C2F_DIR)/third_party/opus
C2F_SRCS   = $(C2F_DIR)/src/c2fargan.c $(C2F_DIR)/src/c2f_features.c \
             $(C2F_DIR)/model/fargan_data.c \
             $(C2F_OPUS)/dnn/fargan.c $(C2F_OPUS)/dnn/nnet.c \
             $(C2F_OPUS)/dnn/nnet_default.c $(C2F_OPUS)/dnn/parse_lpcnet_weights.c
C2F_OBJS   = $(C2F_SRCS:.c=.o)
C2F_INC    = -I$(C2F_DIR) -I$(C2F_DIR)/include -I$(C2F_DIR)/src -I$(C2F_DIR)/model \
             -I$(C2F_OPUS) -I$(C2F_OPUS)/dnn -I$(C2F_OPUS)/celt -I$(C2F_OPUS)/include \
             -Icodec2mod -Icodec2mod/inc
C2F_CFLAGS = -Wall -Wextra -O3 -DSUPPRESS_PERF_WARNINGS $(C2F_INC)

ifneq (,$(filter x86_64 amd64 AMD64,$(shell uname -m)))
  C2F_CFLAGS += -mavx2 -mfma
endif

# A stock libcodec2 build cannot answer the FARGAN sources' calls into the
# modified codec's internals, so the option turns itself off rather than
# failing the build - the sim and console builds stay usable either way.
ifeq ($(NEURAL),1)
  ifneq ($(CODEC2_BACKEND),mod)
    NEURAL := 0
    $(warning NEURAL=1 needs CODEC2_BACKEND=mod: building without the neural receiver)
  endif
endif

ifeq ($(NEURAL),1)
  CFLAGS   += -DFARGAN_RX $(C2F_INC)
  CXXFLAGS += -DFARGAN_RX $(C2F_INC)
  EXTRA_OBJS += $(C2F_OBJS)
endif


# Embed the web assets
ifeq ($(EMBED),1)
CXXFLAGS  += -DEMBED_WWW
GENERATED  = www_assets.h
endif

OBJS = $(SRCS:.cpp=.o) $(CSRCS:.c=.o)
HDRS = $(wildcard *.h) $(GENERATED)

# Any header change rebuilds everything (there are only a few files)
$(OBJS): $(HDRS)

# ---------------------------------------------------------------------------
.PHONY: all sim console bitexact clean help assets

all: $(TARGET)

sim:
	$(MAKE) SIM=1 PULSE=0 PLUTO=0 LIME=0 $(TARGET)

console:
	$(MAKE) CONSOLE=1 WEB=0 $(TARGET)

$(TARGET): $(GENERATED) $(OBJS) $(EXTRA_OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) $(EXTRA_OBJS) -o $@ $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# The vendored modified codec (always compiled with its symbols renamed)
codec2mod/%.o: codec2mod/%.c
	$(CC) $(CODEC2_MOD_CFLAGS) -c $< -o $@

# The vendored FARGAN sources.  Their own flags, because -O3 is worth a lot on
# this code, and because they call the modified Codec2's internals by their real
# names instead of being renamed like it.
$(C2F_DIR)/%.o: $(C2F_DIR)/%.c
	$(CC) $(C2F_CFLAGS) -c $< -o $@

$(C2F_OPUS)/dnn/%.o: $(C2F_OPUS)/dnn/%.c
	$(CC) $(C2F_CFLAGS) -c $< -o $@

# Compare the vendored modified Codec2 against the stock libcodec2.
#
# Two separate binaries, one codec each, on purpose: libcodec2 exports internal
# symbols (kiss_fftr, analyse_one_frame, ...) under the same names as the
# modified codec, and an executable's definitions win when the shared library
# resolves its own calls - link both and the stock encoder jumps into the mod's
# internals (segfault).  tools/codec2_compare.py diffs the results.
bitexact: $(CODEC2_MOD_OBJS)
	@mkdir -p tests
	$(CC) -Wall -Wextra -O3 -I$(CODEC2_PREFIX)/include -I$(CODEC2_PREFIX)/include/codec2 \
	    -o tests/codec2_frame_stock tests/codec2_frame_codec.c -lcodec2 -lm
	$(CC) -Wall -Wextra -O3 -Icodec2mod -Icodec2mod/inc -DCODEC2_MOD \
	    -o tests/codec2_frame_mod tests/codec2_frame_codec.c $(CODEC2_MOD_OBJS) -lm
	@echo "built tests/codec2_frame_stock and tests/codec2_frame_mod"
	@echo "usage: make bitexact-check INPUT=~/codec2/raw/hts1a.raw"

# Run both codecs over one file and compare bitstreams + decoded audio
bitexact-check: bitexact
	@test -n "$(INPUT)" || { echo "usage: make bitexact-check INPUT=/path/to/file.raw"; exit 1; }
	tests/codec2_frame_stock $(INPUT) /tmp/c2_stock.c2 /tmp/c2_stock.raw
	tests/codec2_frame_mod   $(INPUT) /tmp/c2_mod.c2   /tmp/c2_mod.raw
	python3 tools/codec2_compare.py /tmp/c2_stock.c2 /tmp/c2_mod.c2 \
	    --stock-raw /tmp/c2_stock.raw --mod-raw /tmp/c2_mod.raw

# Generate the embedded asset table from www/
www_assets.h: $(wildcard www/*) tools/embed_www.py
	python3 tools/embed_www.py www > $@

assets:
	python3 tools/embed_www.py www > www_assets.h

clean:
	rm -f $(TARGET) *.o www_assets.h
	rm -f tests/codec2_frame_stock tests/codec2_frame_mod
	rm -f codec2mod/*.o codec2mod/src/*.o
	rm -f $(C2F_OBJS)

help:
	@echo "make            web UI, all available radio and audio back ends"
	@echo "make sim        web UI with the simulated radio (no hardware needed)"
	@echo "make console    legacy ncurses UI"
	@echo "make bitexact-check INPUT=file.raw   modified vs stock Codec2 comparison"
	@echo "options: PULSE=0 PLUTO=0 LIME=0 SIM=1 CONSOLE=1 WEB=0 EMBED=0 DEBUG=1"
	@echo "         CODEC2_BACKEND=mod   (use the vendored modified Codec2)"
	@echo "         NEURAL=0             (leave the FARGAN neural receiver out)"
	@echo "run:     ./$(TARGET) -r sim -w 8080"
