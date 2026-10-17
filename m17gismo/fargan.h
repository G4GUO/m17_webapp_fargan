// fargan.h
//
// Receive-only FARGAN synthesis for the station's Codec2 3200 frames: the same
// 8-byte frames, decoded to 16 kHz speech instead of 8 kHz.  Nothing about the
// transmitted signal changes, so this is a per-station option.
//
// Built in with make NEURAL=1 (the default); with NEURAL=0 every function here
// is a stub and the program behaves exactly as before.
#ifndef FARGAN_H
#define FARGAN_H

#include <stdint.h>
#include <stdbool.h>

// Is the neural receiver compiled in, and switched on?
bool fargan_rx_compiled(void);
bool fargan_rx_enabled(void);

// Switch it on or off.  Persisted by the caller (mmi.cpp writes config.txt).
void fargan_rx_set(bool on);

// Forget all history - the API asks for this at the start of each transmission.
void fargan_rx_reset(void);

// One 8-byte Codec2 3200 frame in.  out160 gets 160 samples at 8 kHz (what the
// station's audio path expects today); out320 gets 320 samples at 16 kHz, for
// when the audio path itself is widened.
void fargan_decode_to_8k(const uint8_t *bits8, int16_t *out160);
void fargan_decode_to_16k(const uint8_t *bits8, int16_t *out320);

#endif // FARGAN_H
