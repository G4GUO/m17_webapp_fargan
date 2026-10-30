/*
 * KISS framing for M17-KISS basic packet mode.  See kiss.h.
 *
 * BSD Zero Clause License.  See LICENSE.
 */
#include <string.h>

#include "kiss.h"

void kiss_decoder_init(kiss_decoder *d, uint8_t *buf, size_t cap)
{
    memset(d, 0, sizeof *d);
    d->buf   = buf;
    d->cap   = cap;
    d->state = KISS_HUNT;
}

static void append(kiss_decoder *d, uint8_t byte)
{
    if (d->len < d->cap)
        d->buf[d->len++] = byte;
    else
        d->overrun = 1; /* keep consuming until FEND, but deliver nothing */
}

/* A FEND arrived: close the frame if one was in progress. */
static int emit(kiss_decoder *d, kiss_frame *out)
{
    if (d->state == KISS_HUNT)
        return 0; /* empty frame between back-to-back FENDs */

    if (out != NULL) {
        out->port    = d->port;
        out->command = d->command;
        out->overrun = d->overrun;
        out->len     = d->overrun ? 0 : d->len;
    }

    d->state   = KISS_HUNT;
    d->len     = 0;
    d->overrun = 0;
    return 1;
}

int kiss_decoder_byte(kiss_decoder *d, uint8_t byte, kiss_frame *out)
{
    if (byte == KISS_FEND)
        return emit(d, out);

    switch (d->state) {
    case KISS_HUNT:
        d->port    = (uint8_t)(byte >> 4);
        d->command = (uint8_t)(byte & 0x0Fu);
        d->state   = KISS_BODY;
        return 0;

    case KISS_ESC:
        /* Only 0xDC/0xDD are legal.  Anything else is malformed; recover by
         * applying the inverse of the escape transform rather than losing the
         * frame, which is what other KISS implementations do. */
        d->state = KISS_BODY;
        if (byte == KISS_TFEND)
            append(d, KISS_FEND);
        else if (byte == KISS_TFESC)
            append(d, KISS_FESC);
        else
            append(d, (uint8_t)(byte ^ 0x20u));
        return 0;

    case KISS_BODY:
    default:
        if (byte == KISS_FESC) {
            d->state = KISS_ESC;
            return 0;
        }
        append(d, byte);
        return 0;
    }
}

size_t kiss_decoder_push(kiss_decoder *d, const uint8_t *bytes, size_t n,
                         kiss_frame_cb cb, void *ctx)
{
    size_t count = 0;

    for (size_t i = 0; i < n; i++) {
        kiss_frame frame;

        if (kiss_decoder_byte(d, bytes[i], &frame)) {
            count++;
            if (cb != NULL)
                cb(ctx, &frame, d->buf);
        }
    }
    return count;
}

size_t kiss_encode(uint8_t port, uint8_t command,
                   const uint8_t *data, size_t len,
                   uint8_t *out, size_t outcap)
{
    size_t extra = 0;

    for (size_t i = 0; i < len; i++)
        if (data[i] == KISS_FEND || data[i] == KISS_FESC)
            extra++;

    /* guard against wraparound before adding anything up */
    if (len > outcap || extra > outcap)
        return 0;

    size_t need = len + extra + 3; /* open FEND + header byte + close FEND */
    if (need > outcap)
        return 0;

    size_t j = 0;
    out[j++] = KISS_FEND;
    out[j++] = (uint8_t)(((port & 0x0Fu) << 4) | (command & 0x0Fu));

    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];

        if (b == KISS_FEND) {
            out[j++] = KISS_FESC;
            out[j++] = KISS_TFEND;
        } else if (b == KISS_FESC) {
            out[j++] = KISS_FESC;
            out[j++] = KISS_TFESC;
        } else {
            out[j++] = b;
        }
    }

    out[j++] = KISS_FEND;
    return j;
}
