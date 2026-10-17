// fargan.h
//
// Receive-only FARGAN synthesis for the station's Codec2 3200 frames: the same
// 8-byte frames, decoded to speech at 16 kHz instead of 8 kHz.  Nothing about
// the transmitted signal changes, so this is a per-station option.
//
// The synthesis runs on its own thread.  A received frame is handed over with
// fargan_rx_push() and the caller takes whatever finished audio is ready with
// fargan_rx_pull(), so neither the radio thread nor the reflector thread ever
// waits for the vocoder - it costs about half a Pi 4 core, which is not time the
// demodulator can spare.  The queue adds one frame of latency (20 ms).
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

// Forget all history - applied at the next frame, on the synthesis thread.
void fargan_rx_reset(void);

// Hand a received 8-byte Codec2 3200 frame to the synthesis thread.  Returns
// immediately; if the queue is full the oldest frame is dropped, because live
// audio matters more than filling in the past.
void fargan_rx_push(const uint8_t *bits8);

// Take one 20 ms block if the synthesis thread has one ready: 160 samples at
// 8 kHz, which is what the station's audio path plays, and - when the caller
// wants it - the same audio at FARGAN's own 16 kHz, which is what a recording
// should keep.  Either pointer may be NULL.  Returns 0 when nothing is ready,
// which the caller rides out rather than waiting for.
int fargan_rx_pull(int16_t *out8k, int16_t *out16k);

// Queue and drop counts, for the state the web page shows.
void fargan_rx_stats(unsigned long *queued, unsigned long *decoded,
                     unsigned long *dropped, unsigned long *dry);

#endif // FARGAN_H
