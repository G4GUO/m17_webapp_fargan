/*
 * KISS framing for M17-KISS basic packet mode.
 *
 * A KISS stream is a sequence of frames delimited by 0xC0 (FEND).  Inside a
 * frame the bytes 0xC0 and 0xDB are escaped as 0xDB 0xDC and 0xDB 0xDD.  The
 * first byte after a FEND is (port << 4) | command; for basic packet mode the
 * port is 0 and the command is 0.
 *
 * The decoder is transport agnostic and byte-at-a-time: it survives partial
 * records, coalesced records and records split across TCP reads.  It makes no
 * attempt to interpret AX.25 or M17; that is the caller's business.
 *
 * BSD Zero Clause License.  See LICENSE.
 */
#ifndef KISS_H
#define KISS_H

#include <stddef.h>
#include <stdint.h>

#define KISS_FEND  0xC0u
#define KISS_FESC  0xDBu
#define KISS_TFEND 0xDCu
#define KISS_TFESC 0xDDu

/* Largest application payload an M17 packet may carry (823 bytes total,
 * including the 1-byte type identifier).  The codec does not enforce it;
 * the radio side must, because KISS has no way to report a drop. */
#define KISS_MAX_PAYLOAD 823

typedef enum {
    KISS_HUNT = 0, /* between frames, looking for FEND or a header byte */
    KISS_BODY,     /* inside a frame body */
    KISS_ESC      /* saw FESC, next byte is the escaped value */
} kiss_state;

typedef struct {
    kiss_state state;
    uint8_t    port;
    uint8_t    command;
    uint8_t   *buf;   /* caller supplied storage for the payload */
    size_t     cap;
    size_t     len;
    int        overrun; /* frame exceeded cap; it will be discarded */
} kiss_decoder;

typedef struct {
    uint8_t port;
    uint8_t command;
    size_t  len;     /* 0 when overrun is set */
    int     overrun; /* frame was longer than the buffer and was dropped */
} kiss_frame;

/* Called once per completed frame.  `data` points at the decoder's buffer,
 * valid only for the duration of the call (until the next byte is pushed). */
typedef void (*kiss_frame_cb)(void *ctx, const kiss_frame *frame, const uint8_t *data);

void   kiss_decoder_init(kiss_decoder *d, uint8_t *buf, size_t cap);

/* Feed one byte.  Returns 1 and fills *out (which may be NULL) when a frame
 * completed on this byte, 0 otherwise.  Sequences of FENDs produce no frames. */
int    kiss_decoder_byte(kiss_decoder *d, uint8_t byte, kiss_frame *out);

/* Feed a block, invoking cb for each completed frame.  Returns the number of
 * frames delivered. */
size_t kiss_decoder_push(kiss_decoder *d, const uint8_t *bytes, size_t n,
                         kiss_frame_cb cb, void *ctx);

/* Write one frame into out.  Returns the number of bytes written, or 0 if it
 * would not fit in outcap (nothing is written in that case). */
size_t kiss_encode(uint8_t port, uint8_t command,
                   const uint8_t *data, size_t len,
                   uint8_t *out, size_t outcap);

#endif /* KISS_H */
