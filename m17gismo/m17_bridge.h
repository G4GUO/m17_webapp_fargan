/*
 * KISS-to-burst bridge: the queue and the channel-access state machine.
 *
 * This module holds no I/O, no clock and no radio.  Time, the channel-busy
 * indication, the mic PTT state and a random word are all supplied by the
 * caller, which makes the behaviour testable without hardware.
 *
 * Behaviour (see docs/design.md):
 *   - frames are opaque; nothing inside them is parsed
 *   - one frame in flight, immutable once queued
 *   - a frame identical to one already queued is dropped (a retransmit)
 *   - queue full: the new frame is dropped, never the oldest
 *   - voice transmission always wins; a burst in flight always finishes
 *   - optional quiet period after channel or voice activity, so a data burst
 *     does not drop into the gap between overs
 *
 * BSD Zero Clause License.  See LICENSE.
 */
#ifndef M17_BRIDGE_H
#define M17_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#include "m17_packet.h"

#define M17_BRIDGE_QUEUE_MAX 8

typedef struct {
    unsigned queue_depth;    /* 1..M17_BRIDGE_QUEUE_MAX, default 4 */
    unsigned slot_ms;        /* access slot, default 40 (one M17 frame) */
    unsigned max_slots;      /* backoff is 0..max_slots slots, default 3 */
    unsigned guard_ms;       /* DCD ignore time after our own burst, default 20 */
    unsigned quiet_ms;       /* required channel quiet before sending, default 0 */
} m17_bridge_config;

typedef enum {
    M17_BRIDGE_IDLE = 0,
    M17_BRIDGE_BACKOFF,
    M17_BRIDGE_TX,      /* burst handed to the caller, awaiting m17_bridge_sent */
    M17_BRIDGE_GUARD    /* our own key-down tail; DCD is not trusted yet */
} m17_bridge_state;

typedef struct {
    uint8_t data[M17_MAX_APP_BYTES];
    size_t  len;
} m17_bridge_frame;

typedef struct {
    m17_bridge_config cfg;
    m17_bridge_frame  q[M17_BRIDGE_QUEUE_MAX];
    unsigned          count;

    m17_bridge_state  state;
    uint32_t          deadline;
    uint32_t          last_activity;
    int               have_activity;

    unsigned          dropped_dup;
    unsigned          dropped_full;
    unsigned          sent;
} m17_bridge;

void m17_bridge_config_default(m17_bridge_config *cfg);
void m17_bridge_init(m17_bridge *b, const m17_bridge_config *cfg);

/* Returns 1 if the frame was queued, 0 if it was dropped as a duplicate or
 * because the queue is full (see b->dropped_dup / b->dropped_full). */
int  m17_bridge_push(m17_bridge *b, const uint8_t *frame, size_t len);

/* Advance the state machine.
 *   now_ms   monotonic milliseconds
 *   dcd      channel busy (any M17 sync, plus the energy gate)
 *   voice_tx the operator is transmitting voice
 *   rand32   any random word, used only when a new backoff is drawn
 * Returns 1 when the caller should transmit the frame at the head of the
 * queue, then call m17_bridge_sent() once the burst is on the air. */
int  m17_bridge_tick(m17_bridge *b, uint32_t now_ms, int dcd, int voice_tx, uint32_t rand32);

/* The burst for the frame at the head of the queue.  Valid when tick() returned
 * 1, and until it does so again. */
const uint8_t *m17_bridge_head(const m17_bridge *b, size_t *len);

/* Called after the burst has been transmitted (or abandoned). */
void m17_bridge_sent(m17_bridge *b, uint32_t now_ms);

#endif /* M17_BRIDGE_H */
