/*
 * M17 packet mode: Link Setup Data, payload framing and reassembly.
 * See m17_packet.h.
 *
 * BSD Zero Clause License.  See LICENSE.
 */
#include <string.h>

#include "m17_packet.h"

/* ------------------------------------------------------------------ CRC */

uint16_t m17_crc16(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFu;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 8;

        for (int bit = 0; bit < 8; bit++) {
            crc <<= 1;
            if (crc & 0x10000u)
                crc = (crc ^ 0x5935u) & 0xFFFFu;
        }
    }
    return (uint16_t)(crc & 0xFFFFu);
}

/* -------------------------------------------------------------- addresses */

/* Values 0..39 in the M17 alphabet: space, A-Z, 0-9, '-', '/', '.' */
static unsigned alphabet_value(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');

    if (c >= 'A' && c <= 'Z')
        return (unsigned)(c - 'A') + 1;
    if (c >= '0' && c <= '9')
        return (unsigned)(c - '0') + 27;
    if (c == '-')
        return 37;
    if (c == '/')
        return 38;
    if (c == '.')
        return 39;
    return 0; /* space, and anything invalid */
}

void m17_addr_from_u48(uint64_t value, uint8_t out[M17_ADDR_BYTES])
{
    for (int i = M17_ADDR_BYTES - 1; i >= 0; i--) {
        out[i] = (uint8_t)(value & 0xFFu);
        value >>= 8;
    }
}

uint64_t m17_addr_to_u48(const uint8_t in[M17_ADDR_BYTES])
{
    uint64_t value = 0;

    for (int i = 0; i < M17_ADDR_BYTES; i++)
        value = (value << 8) | in[i];
    return value;
}

int m17_addr_encode(const char *text, uint8_t out[M17_ADDR_BYTES])
{
    size_t   n;
    uint64_t word = 0;

    if (text == NULL || (n = strlen(text)) == 0 || n > M17_MAX_TEXT)
        return 0;

    /* Encoded backwards: the first character is the least significant base-40
     * digit, so trailing spaces contribute nothing. */
    for (int i = M17_MAX_TEXT - 1; i >= 0; i--) {
        char c = ((size_t)i < n) ? text[i] : ' ';
        word = word * 40u + alphabet_value(c);
    }

    m17_addr_from_u48(word, out);
    return 1;
}

/* -------------------------------------------------------------------- LSF */

void m17_lsd_build(uint8_t lsd[M17_LSD_BYTES],
                   const uint8_t dst[M17_ADDR_BYTES],
                   const uint8_t src[M17_ADDR_BYTES],
                   uint16_t type,
                   const uint8_t meta[M17_META_BYTES])
{
    memcpy(&lsd[0], dst, M17_ADDR_BYTES);
    memcpy(&lsd[6], src, M17_ADDR_BYTES);
    lsd[12] = (uint8_t)(type >> 8);
    lsd[13] = (uint8_t)(type & 0xFFu);
    if (meta != NULL)
        memcpy(&lsd[14], meta, M17_META_BYTES);
    else
        memset(&lsd[14], 0, M17_META_BYTES);

    uint16_t crc = m17_crc16(lsd, M17_LSD_BYTES - 2);
    lsd[28] = (uint8_t)(crc >> 8);
    lsd[29] = (uint8_t)(crc & 0xFFu);
}

/* ---------------------------------------------------------------- payload */

size_t m17_pkt_build(m17_pkt_burst *burst, uint8_t type,
                     const uint8_t *payload, size_t len)
{
    uint8_t buf[M17_MAX_APP_BYTES + 2];

    /* the type specifier is variable length in general; only the 1-byte form
     * (values 0..0x7F) is supported here */
    if (type > 0x7Fu)
        return 0;
    if (len > (size_t)(M17_MAX_APP_BYTES - 1))
        return 0;

    size_t app_len = len + 1;
    size_t total   = app_len + 2;
    size_t frames  = (total + M17_CHUNK_BYTES - 1) / M17_CHUNK_BYTES;

    if (frames > M17_MAX_FRAMES)
        return 0;

    buf[0] = type;
    if (len > 0)
        memcpy(&buf[1], payload, len);

    uint16_t crc = m17_crc16(buf, app_len);
    buf[app_len]     = (uint8_t)(crc >> 8);
    buf[app_len + 1] = (uint8_t)(crc & 0xFFu);

    for (size_t i = 0; i < frames; i++) {
        m17_pkt_frame *f = &burst->frame[i];
        size_t         off = i * M17_CHUNK_BYTES;
        size_t         n   = total - off;

        if (n > M17_CHUNK_BYTES)
            n = M17_CHUNK_BYTES;

        memset(f->data, 0, M17_CHUNK_BYTES);
        memcpy(f->data, &buf[off], n);

        if (i == frames - 1)
            f->meta = (uint8_t)(0x80u | ((n & 0x1Fu) << 2));  /* EOF + byte count */
        else
            f->meta = (uint8_t)((i & 0x1Fu) << 2);            /* frame counter */
    }

    burst->count = frames;
    return frames;
}

/* ------------------------------------------------------------ reassembly */

void m17_rx_reset(m17_rx_assembler *a)
{
    a->len  = 0;
    a->next = 0;
}

m17_pkt_status m17_rx_chunk(m17_rx_assembler *a, const m17_pkt_frame *f)
{
    unsigned eof   = (f->meta >> 7) & 1u;
    unsigned field = (f->meta >> 2) & 0x1Fu;

    if (!eof) {
        if (field != a->next)
            return M17_PKT_ERROR; /* out of sequence */
        if (a->len + M17_CHUNK_BYTES > sizeof a->data)
            return M17_PKT_ERROR;
        memcpy(&a->data[a->len], f->data, M17_CHUNK_BYTES);
        a->len += M17_CHUNK_BYTES;
        a->next++;
        return M17_PKT_NEED_MORE;
    }

    /* final frame: the field is the number of valid bytes, 1..25 */
    if (field == 0 || field > M17_CHUNK_BYTES)
        return M17_PKT_ERROR;
    if (a->len + field > sizeof a->data)
        return M17_PKT_ERROR;
    memcpy(&a->data[a->len], f->data, field);
    a->len += field;

    if (a->len < 3)
        return M17_PKT_ERROR; /* needs at least a type byte and the CRC */

    size_t   payload_len = a->len - 2;
    uint16_t want = (uint16_t)((a->data[payload_len] << 8) | a->data[payload_len + 1]);
    uint16_t got  = m17_crc16(a->data, payload_len);

    a->len = payload_len;
    return (want == got) ? M17_PKT_COMPLETE : M17_PKT_ERROR;
}
