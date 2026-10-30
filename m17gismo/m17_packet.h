/*
 * M17 packet mode: Link Setup Data, payload framing and reassembly.
 *
 * A packet transmission is LSF (40 ms) followed by 1..33 packet frames and an
 * EoT.  The application data is a 1..n byte data type specifier followed by the
 * payload, with a 16-bit CRC appended over the whole thing; the result is split
 * into 25-byte chunks.  Each chunk carries a 6-bit metadata field: EOF in bit 7,
 * and in bits 6..2 either the frame counter (EOF clear) or the number of valid
 * bytes in the final frame (EOF set).
 *
 * The modulation chain (convolutional coding, puncturing, interleaving,
 * randomising) belongs to the radio; this library stops at the 25+1 byte frame.
 *
 * BSD Zero Clause License.  See LICENSE.
 */
#ifndef M17_PACKET_H
#define M17_PACKET_H

#include <stddef.h>
#include <stdint.h>

#define M17_CHUNK_BYTES   25   /* payload bytes per packet frame */
#define M17_MAX_FRAMES    33   /* 825 bytes on air, including the CRC */
#define M17_MAX_APP_BYTES 823  /* type specifier + payload */
#define M17_LSD_BYTES     30   /* DST, SRC, TYPE, META, CRC */
#define M17_META_BYTES    14
#define M17_ADDR_BYTES    6
#define M17_MAX_TEXT      9    /* characters that fit a 48-bit address */

#define M17_ADDR_BROADCAST 0xFFFFFFFFFFFFull

/* Reserved packet data type specifiers (spec 3.0 Table 3.15). */
#define M17_TYPE_RAW   0x00
#define M17_TYPE_AX25  0x01
#define M17_TYPE_APRS  0x02

/* Packet LSF TYPE for basic mode: CAN 0, packet, no encryption. */
#define M17_PACKET_TYPE_CAN0 0x0000

typedef struct {
    uint8_t data[M17_CHUNK_BYTES]; /* null padded beyond the valid length */
    uint8_t meta;                  /* EOF bit 7, counter or byte count bits 6..2 */
} m17_pkt_frame;

typedef struct {
    m17_pkt_frame frame[M17_MAX_FRAMES];
    size_t        count;
} m17_pkt_burst;

typedef enum {
    M17_PKT_NEED_MORE = 0,
    M17_PKT_COMPLETE  = 1,
    M17_PKT_ERROR     = -1
} m17_pkt_status;

typedef struct {
    uint8_t  data[M17_MAX_APP_BYTES + 2];
    size_t   len;
    unsigned next; /* frame counter expected next */
} m17_rx_assembler;

/* M17 CRC-16: polynomial 0x5935, initial value 0xFFFF, MSB first, no final
 * inversion.  Identical to libm17's CRC_M17() and to the radio's
 * m17_crc_array_encode(). */
uint16_t m17_crc16(const uint8_t *data, size_t len);

/* Encode a callsign into 6 bytes, big endian.  Lower case is folded to upper
 * case and characters outside the alphabet encode as space.  Returns 0 if the
 * text is empty or longer than 9 characters. */
int      m17_addr_encode(const char *text, uint8_t out[M17_ADDR_BYTES]);
void     m17_addr_from_u48(uint64_t value, uint8_t out[M17_ADDR_BYTES]);
uint64_t m17_addr_to_u48(const uint8_t in[M17_ADDR_BYTES]);

/* Build the 30 bytes of Link Setup Data.  `type` is the 16-bit field in the
 * order it appears on air: high byte holds CAN (bits 3..0) and the signed
 * flag (bit 4), low byte holds P/S (bit 0), data type (bits 2..1) and the
 * encryption fields.  Packet mode with CAN 0 is M17_PACKET_TYPE_CAN0. */
void m17_lsd_build(uint8_t lsd[M17_LSD_BYTES],
                   const uint8_t dst[M17_ADDR_BYTES],
                   const uint8_t src[M17_ADDR_BYTES],
                   uint16_t type,
                   const uint8_t meta[M17_META_BYTES]);

/* Split one packet into frames.  Returns the number of frames written, or 0 if
 * it does not fit (type > 0x7F, or payload longer than 822 bytes). */
size_t m17_pkt_build(m17_pkt_burst *burst, uint8_t type,
                     const uint8_t *payload, size_t len);

void           m17_rx_reset(m17_rx_assembler *a);
m17_pkt_status m17_rx_chunk(m17_rx_assembler *a, const m17_pkt_frame *f);
/* On M17_PKT_COMPLETE, a->data[0] is the type specifier, a->len counts it, and
 * the CRC has been removed. */

#endif /* M17_PACKET_H */
