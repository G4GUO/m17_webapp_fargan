/*
 * AX.25 over M17 packet mode: the bridge between a KISS host (BPQ or similar)
 * and the radio.
 *
 * The protocol code lives in kiss.c, m17_packet.c and m17_bridge.c, which are
 * taken verbatim from the G4GUO/m17-ax25 repository; this file is only the
 * glue: a listening socket, a queue fed by it, and the point in the receive
 * loop at which a queued packet may be transmitted.
 *
 * Off by default.  With "data off" nothing is opened, nothing is queued and
 * the voice path is untouched.
 *
 * BSD Zero Clause License.  See LICENSE.
 */
#ifndef M17_AX25_H
#define M17_AX25_H

#ifdef __cplusplus
extern "C" {
#endif

/* One-off setup: reads nothing, opens nothing. */
void m17_ax25_init(void);

/* Packet mode on or off.  Turning it on opens the KISS port; turning it off
 * closes it and drops anything queued. */
void m17_ax25_set_enabled(int on);
int  m17_ax25_enabled(void);

/* Loopback: build the packet and feed it straight back into the receive path
 * instead of transmitting.  For testing BPQ without a radio. */
void m17_ax25_set_loopback(int on);
int  m17_ax25_loopback(void);

/* KISS TCP port.  Returns 0 if the new port cannot be opened. */
int  m17_ax25_set_port(unsigned port);
unsigned m17_ax25_port(void);

/* Source address for the M17 link setup frame.  Empty means "use the station
 * source address (sa)".  Returns 0 if the callsign does not fit. */
int  m17_ax25_set_call(const char *call);
const char *m17_ax25_call(void);

/* Which M17 mode carries the AX.25 frames: 1 = stream (default, works on a
 * marginal link), 0 = packet (the specified way to carry data).  Runtime. */
void m17_ax25_set_framing(int stream);
int  m17_ax25_framing(void);

/* Called from the receive path whenever a frame is heard, for channel busy. */
void m17_ax25_note_activity(void);

/* Feed a packet burst through the real receive chain from the symbol level up,
 * and report whether the frame comes back intact.  No radio involved. */
void m17_ax25_wire_test(void);

/* Called from valid_packet_received(): type byte plus AX.25 frame. */
void m17_ax25_rx_packet(const unsigned char *data, int len);

/* Called from decode_stream_frame() with each 16-byte stream payload when the
 * link setup frame carries our tag.  Frames are carried in stream mode, whose
 * convolutional coding and repeated LSF survive a link that packet mode does
 * not. */
void m17_ax25_rx_stream(const unsigned char *data, int len);

/* True if the current link setup frame's meta field marks it as ours. */
int  m17_ax25_is_stream_tag(const unsigned char *meta);

/* Called from the receive loop.  Returns 1 if it transmitted (or looped back)
 * a packet, in which case the caller should go round again. */
int  m17_ax25_service(void);

/* One line of state and counters, for the "data status" command. */
void m17_ax25_status(char *out, unsigned n);

#ifdef __cplusplus
}
#endif

#endif /* M17_AX25_H */
