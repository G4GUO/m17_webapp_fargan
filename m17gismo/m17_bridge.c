/*
 * KISS-to-burst bridge: queue and channel access.  See m17_bridge.h.
 *
 * BSD Zero Clause License.  See LICENSE.
 */
#include <string.h>

#include "m17_bridge.h"

void m17_bridge_config_default(m17_bridge_config *cfg)
{
    cfg->queue_depth = 4;
    cfg->slot_ms     = 40;  /* one M17 frame */
    cfg->max_slots   = 3;
    cfg->guard_ms    = 20;
    cfg->quiet_ms    = 0;
}

void m17_bridge_init(m17_bridge *b, const m17_bridge_config *cfg)
{
    memset(b, 0, sizeof *b);

    if (cfg != NULL)
        b->cfg = *cfg;
    else
        m17_bridge_config_default(&b->cfg);

    if (b->cfg.queue_depth == 0)
        b->cfg.queue_depth = 1;
    if (b->cfg.queue_depth > M17_BRIDGE_QUEUE_MAX)
        b->cfg.queue_depth = M17_BRIDGE_QUEUE_MAX;

    b->state = M17_BRIDGE_IDLE;
}

/* Wrap-safe "has this timestamp been reached". */
static int reached(uint32_t now, uint32_t when)
{
    return (int32_t)(now - when) >= 0;
}

int m17_bridge_push(m17_bridge *b, const uint8_t *frame, size_t len)
{
    if (frame == NULL || len == 0 || len > M17_MAX_APP_BYTES)
        return 0;

    /* A retransmission of something still queued needs no second burst. */
    for (unsigned i = 0; i < b->count; i++) {
        if (b->q[i].len == len && memcmp(b->q[i].data, frame, len) == 0) {
            b->dropped_dup++;
            return 0;
        }
    }

    if (b->count >= b->cfg.queue_depth) {
        b->dropped_full++;
        return 0;
    }

    memcpy(b->q[b->count].data, frame, len);
    b->q[b->count].len = len;
    b->count++;
    return 1;
}

static void start_backoff(m17_bridge *b, uint32_t now_ms, uint32_t rand32)
{
    unsigned slots = (b->cfg.max_slots == 0) ? 0 : (rand32 % (b->cfg.max_slots + 1));

    b->state    = M17_BRIDGE_BACKOFF;
    b->deadline = now_ms + slots * b->cfg.slot_ms;
}

int m17_bridge_tick(m17_bridge *b, uint32_t now_ms, int dcd, int voice_tx, uint32_t rand32)
{
    /* Our own transmission is in flight: nothing may interrupt it. */
    if (b->state == M17_BRIDGE_TX)
        return 0;

    /* Voice has priority to start.  Anything heard on the channel also counts
     * as activity for the quiet-period rule, because a paused QSO is still a
     * QSO. */
    if (voice_tx || dcd) {
        b->last_activity = now_ms;
        b->have_activity = 1;
        if (b->state == M17_BRIDGE_BACKOFF)
            b->state = M17_BRIDGE_IDLE; /* re-draw when the channel clears */
        return 0;
    }

    if (b->state == M17_BRIDGE_GUARD) {
        if (!reached(now_ms, b->deadline))
            return 0;
        b->state = M17_BRIDGE_IDLE;
    }

    if (b->count == 0) {
        b->state = M17_BRIDGE_IDLE;
        return 0;
    }

    if (b->cfg.quiet_ms != 0 && b->have_activity &&
        (uint32_t)(now_ms - b->last_activity) < b->cfg.quiet_ms)
        return 0;

    if (b->state == M17_BRIDGE_IDLE)
        start_backoff(b, now_ms, rand32);

    if (b->state == M17_BRIDGE_BACKOFF && reached(now_ms, b->deadline)) {
        b->state = M17_BRIDGE_TX;
        return 1;
    }

    return 0;
}

const uint8_t *m17_bridge_head(const m17_bridge *b, size_t *len)
{
    if (b->count == 0)
        return NULL;
    if (len != NULL)
        *len = b->q[0].len;
    return b->q[0].data;
}

void m17_bridge_sent(m17_bridge *b, uint32_t now_ms)
{
    if (b->count == 0) {
        b->state = M17_BRIDGE_IDLE;
        return;
    }

    b->count--;
    if (b->count > 0)
        memmove(&b->q[0], &b->q[1], b->count * sizeof b->q[0]);
    b->sent++;

    b->state    = M17_BRIDGE_GUARD;
    b->deadline = now_ms + b->cfg.guard_ms;
}
