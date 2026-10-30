/*
 * AX.25 over M17 packet mode: KISS socket, queue and transmit point.
 * See m17_ax25.h for the interface and G4GUO/m17-ax25 for the protocol code.
 *
 * BSD Zero Clause License.  See LICENSE.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#include "m17defines.h"
#include "ui.h"

extern "C" {
#include "kiss.h"
#include "m17_bridge.h"
#include "m17_packet.h"
}

#include "m17_ax25.h"

/* m17_tx_routines.cpp: split a packet into 25-byte frames and modulate them.
 * Not in m17defines.h, and it needs a buffer with room for the two CRC bytes
 * it appends, so callers pass a copy. */
void m17_send_packet_frames(uint8_t *packet, int len);

/* Frame builders in m17_tx_routines.cpp.  The wire test needs them directly so
 * it can intercept the dibits instead of transmitting them. */
int m17_fmt_add_link_setup_frame(uint8_t *dibits, uint48_t dest, uint48_t src, M17Type type, uint8_t *meta);
int m17_fmt_add_packet(uint8_t *dibits, int len, bool eof, uint8_t nf, uint8_t *payload);

#define AX25_QUEUE_BYTES  (M17_MAX_APP_BYTES + 2)
#define AX25_DCD_MS       100u   /* a frame every 40 ms while a burst is up */
#define AX25_SLOT_MS      40u    /* one M17 frame */

/* Preamble frames before the LSF, matching the voice path.  Longer preambles
 * were tried up to a full second and made reception worse, not better: the
 * receiver locks onto a preamble frame instead of the LSF, so its symbol
 * alignment is a fraction out and the LSF decodes to the same wrong bits every
 * burst.  Two frames is what the modem is proven to decode. */
#define AX25_PREAMBLES    2

/* Zero-filled stream frames after the data, to flush the decoder. */
#define AX25_TAIL_FRAMES  3

/* How many times the link setup frame is repeated at the head of a burst.
 * The receiver's timing loop is data directed, so the alternating preamble
 * gives it nothing to work with: it settles during the first data frame, which
 * is the LSF, and therefore loses it.  Reference transmitters repeat the LSF
 * for the same reason - a handheld's LSF decodes on the second or third copy
 * every time.  One copy means our own bursts always lose it. */
#define AX25_LSF_REPEATS  3

/* How many times the burst is sent in one key-up.  Three turns a roughly one in
 * five chance of a clean LSF into better than even odds without waiting for T1. */
#define AX25_BURST_PASSES 3

static int             m_on;

/* Wire test: capture what the receive chain produces instead of delivering it */
static int             m_wire_active;
static int             m_wire_len;
static uint8_t         m_wire_got[AX25_QUEUE_BYTES];
static int             m_stream = 1;     /* 1 = stream framing, 0 = packet */
static int             m_loopback;
static unsigned        m_port = 8001;
static char            m_call[M17_MAX_TEXT + 1];

static int             m_listen = -1;
static int             m_client = -1;
static pthread_t       m_thread;
static volatile int    m_run;
static kiss_decoder    m_dec;
static uint8_t         m_decbuf[1024];

static m17_bridge      m_bridge;
static pthread_mutex_t m_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t        m_last_activity;

static unsigned        m_rx_frames, m_tx_frames, m_looped, m_rejected;

static uint32_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)ts.tv_nsec / 1000000u);
}

static void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ------------------------------------------------------------------ output */

static void kiss_write(const uint8_t *frame, size_t len)
{
    uint8_t wire[1200];
    size_t  n;

    if (m_client < 0)
        return;

    n = kiss_encode(0, 0, frame, len, wire, sizeof wire);
    if (n == 0)
        return;

    size_t off = 0;
    while (off < n) {
        ssize_t w = send(m_client, wire + off, n - off, MSG_NOSIGNAL);
        if (w > 0)
            off += (size_t)w;
        else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return; /* a busy host: drop rather than block the receiver */
        else
            return;
    }
}

/* --------------------------------------------------------------- framing */

/* A frame from the host: queue it, or loop it straight back. */
static void on_kiss_frame(void *ctx, const kiss_frame *frame, const uint8_t *data)
{
    (void)ctx;

    if (frame->port != 0 || frame->command != 0)
        return; /* KISS parameter commands are not data */

    if (frame->overrun || frame->len == 0) {
        m_rejected++;
        return;
    }

    pthread_mutex_lock(&m_lock);
    int queued = m17_bridge_push(&m_bridge, data, frame->len);
    pthread_mutex_unlock(&m_lock);

    if (!queued)
        m_rejected++;
}

static void close_client(void)
{
    if (m_client >= 0) {
        close(m_client);
        m_client = -1;
    }
}

static void *kiss_thread(void *arg)
{
    (void)arg;

    while (m_run) {
        struct pollfd pfd[2];
        int           n = 0;

        if (m_listen < 0)
            break;

        pfd[n].fd = m_listen;
        pfd[n].events = POLLIN;
        n++;

        if (m_client >= 0) {
            pfd[n].fd = m_client;
            pfd[n].events = POLLIN;
            n++;
        }

        int rc = poll(pfd, (nfds_t)n, 100);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (rc == 0)
            continue;

        if (pfd[0].revents & POLLIN) {
            int fd = accept(m_listen, NULL, NULL);
            if (fd >= 0) {
                if (m_client >= 0)
                    close(m_client); /* one host at a time */
                set_nonblock(fd);
                m_client = fd;
                kiss_decoder_init(&m_dec, m_decbuf, sizeof m_decbuf);
                printf("m17web: AX.25 KISS host connected\n");
                fflush(stdout);
            }
        }

        if (n > 1 && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            uint8_t chunk[256];
            ssize_t r = recv(m_client, chunk, sizeof chunk, 0);

            if (r > 0) {
                kiss_decoder_push(&m_dec, chunk, (size_t)r, on_kiss_frame, NULL);
            } else if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                printf("m17web: AX.25 KISS host disconnected\n");
                fflush(stdout);
                close_client();
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------- lifecycle */

static int open_listener(unsigned port)
{
    struct sockaddr_in sa;
    int                fd;
    int                one = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 2) < 0) {
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    return fd;
}

void m17_ax25_init(void)
{
    m17_bridge_config cfg;

    m17_bridge_config_default(&cfg);
    cfg.queue_depth = 4;
    cfg.slot_ms     = AX25_SLOT_MS;
    cfg.max_slots   = 3;
    cfg.guard_ms    = 20;
    cfg.quiet_ms    = 3000;  /* stay out of the gaps between overs */
    m17_bridge_init(&m_bridge, &cfg);

    m_call[0] = 0;
    m_on      = 0;
}

void m17_ax25_set_enabled(int on)
{
    if (on == m_on)
        return;

    if (on) {
        int fd = open_listener(m_port);
        if (fd < 0) {
            printf("m17web: AX.25 cannot listen on port %u: %s\n", m_port, strerror(errno));
            fflush(stdout);
            return;
        }
        m_listen = fd;
        m_client = -1;
        kiss_decoder_init(&m_dec, m_decbuf, sizeof m_decbuf);
        m_run = 1;
        if (pthread_create(&m_thread, NULL, kiss_thread, NULL) != 0) {
            m_run = 0;
            close(m_listen);
            m_listen = -1;
            printf("m17web: AX.25 cannot start the KISS thread\n");
            fflush(stdout);
            return;
        }
        m_on = 1;
        printf("m17web: AX.25 packet mode on, KISS port %u\n", m_port);
    } else {
        m_run = 0;
        if (m_listen >= 0)
            close(m_listen);      /* wakes poll() out of its wait */
        pthread_join(m_thread, NULL);
        m_listen = -1;
        m_client = -1;
        m_on = 0;

        pthread_mutex_lock(&m_lock);
        m_bridge.count = 0;
        pthread_mutex_unlock(&m_lock);
        printf("m17web: AX.25 packet mode off\n");
    }
    fflush(stdout);
}

int m17_ax25_enabled(void)  { return m_on; }

void m17_ax25_set_loopback(int on) { m_loopback = on ? 1 : 0; }
int  m17_ax25_loopback(void)       { return m_loopback; }
unsigned m17_ax25_port(void)       { return m_port; }

int m17_ax25_set_port(unsigned port)
{
    int was_on;

    if (port < 1 || port > 65535)
        return 0;

    was_on = m_on;
    if (was_on)
        m17_ax25_set_enabled(0);

    unsigned old = m_port;
    m_port = port;

    if (was_on) {
        m17_ax25_set_enabled(1);
        if (!m_on) {          /* could not listen: go back */
            m_port = old;
            m17_ax25_set_enabled(1);
            return 0;
        }
    }
    return 1;
}

int m17_ax25_set_call(const char *call)
{
    size_t n = call ? strlen(call) : 0;

    if (n > M17_MAX_TEXT)
        return 0;
    snprintf(m_call, sizeof m_call, "%s", call ? call : "");
    return 1;
}

const char *m17_ax25_call(void) { return m_call; }

/* Gaussian noise for the wire test (Box-Muller).  rand() is adequate here: it
 * only has to produce a spread, not a cryptographic stream. */
static double wire_gauss(void)
{
    double u1 = ((double)rand() + 1.0) / ((double)RAND_MAX + 2.0);
    double u2 = ((double)rand() + 1.0) / ((double)RAND_MAX + 2.0);
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

/* Convert the modulator's dibits to symbol values, which is what a perfect
 * discriminator would produce: dibit 0/1/2/3 -> +1/+3/-1/-3.  sigma adds
 * Gaussian noise in those units, so the outer level is 3 and noise grows as the
 * signal falls. */
static void wire_symbols(const uint8_t *dibits, float *syms, int n, double sigma)
{
    static const float lut[4] = { 1.0f, 3.0f, -1.0f, -3.0f };
    for (int i = 0; i < n; i++) {
        double s = lut[dibits[i] & 3];
        if (sigma > 0.0)
            s += sigma * wire_gauss();
        syms[i] = (float)s;
    }
}

/* One burst through the real receive chain - frame builder, demapper,
 * de-correlator, de-interleaver, de-puncturer, Viterbi, CRC and packet
 * reassembly - with optional noise.  Returns 1 if the frame comes back intact. */
static int wire_trial(int len, double sigma)
{
    static uint8_t dibits[FRAME_SYM_LENGTH];
    static float   syms[FRAME_SYM_LENGTH];
    static uint8_t pkt[AX25_QUEUE_BYTES + 2];
    uint8_t        meta[M17_META_BYTES];
    M17Type        type;
    int            i;

    /* Same envelope as send_packet(): protocol specifier then the frame. */
    pkt[0] = 0x00;
    for (i = 0; i < len; i++)
        pkt[1 + i] = (uint8_t)(0xA0 + i);

    memset(&type, 0, sizeof type);          /* p_s = 0: packet */
    memset(meta, 0, sizeof meta);

    m_wire_len = -1;
    m_wire_active = 1;

    m17_fmt_add_link_setup_frame(dibits, M17_ADDR_BROADCAST, m17_db_get_src(), type, meta);
    wire_symbols(dibits, syms, FRAME_SYM_LENGTH, sigma);
    m17_rx_parse(syms, 1);

    int plen = len + 1;
    uint16_t crc = m17_crc_array_encode(pkt, plen);
    pkt[plen]     = (uint8_t)(crc >> 8);
    pkt[plen + 1] = (uint8_t)(crc & 0xFF);
    int total = plen + 2;
    int whole = total / 25;
    int left  = total % 25;

    if (left == 0) {
        for (i = 0; i < whole - 1; i++) {
            m17_fmt_add_packet(dibits, 25, false, (uint8_t)i, &pkt[i * 25]);
            wire_symbols(dibits, syms, FRAME_SYM_LENGTH, sigma);
            m17_rx_parse(syms, 3);
        }
        m17_fmt_add_packet(dibits, 25, true, 25, &pkt[(whole - 1) * 25]);
        wire_symbols(dibits, syms, FRAME_SYM_LENGTH, sigma);
        m17_rx_parse(syms, 3);
    } else {
        for (i = 0; i < whole; i++) {
            m17_fmt_add_packet(dibits, 25, false, (uint8_t)i, &pkt[i * 25]);
            wire_symbols(dibits, syms, FRAME_SYM_LENGTH, sigma);
            m17_rx_parse(syms, 3);
        }
        m17_fmt_add_packet(dibits, left, true, (uint8_t)left, &pkt[whole * 25]);
        wire_symbols(dibits, syms, FRAME_SYM_LENGTH, sigma);
        m17_rx_parse(syms, 3);
    }

    m_wire_active = 0;

    int pass = (m_wire_len == len);
    for (i = 0; pass && i < len; i++)
        if (m_wire_got[i] != pkt[1 + i]) pass = 0;
    return pass;
}

/* Clean run, then a noise sweep.  The sweep is the baseline that a better soft
 * metric has to beat: how often a five chunk frame survives at a given noise
 * level.  Sigma is in symbol units, so the outer level is 3. */
void m17_ax25_wire_test(void)
{
    static const double sigmas[] = { 0.70, 0.75, 0.80, 0.85, 0.90 };
    const int trials = 30;
    const int len    = 106;                 /* five chunks: a node sized reply */
    int keep = m17_demap_llr_mode();

    int pass = wire_trial(len, 0.0);
    printf("m17web: wire test %s (clean, %d bytes)\n", pass ? "PASS" : "FAIL", len);
    gui_log_event(pass ? "ok" : "err", "WIRE clean: %s", pass ? "PASS" : "FAIL");

    /* Same frames, same noise seeds per level, both metrics: the only difference
     * is the soft values handed to the Viterbi. */
    for (int mode = 0; mode < 2; mode++) {
        m17_demap_set_llr(mode);
        for (unsigned s = 0; s < sizeof sigmas / sizeof sigmas[0]; s++) {
            int ok = 0;
            for (int t = 0; t < trials; t++)
                if (wire_trial(len, sigmas[s])) ok++;
            printf("m17web: wire sweep metric=%s sigma=%.1f pass=%d/%d\n",
                   mode ? "llr" : "old", sigmas[s], ok, trials);
            gui_log_event("wire", "WIRE %s sigma %.1f: %d/%d",
                          mode ? "llr" : "old", sigmas[s], ok, trials);
        }
    }
    m17_demap_set_llr(keep);
    fflush(stdout);
}

void m17_ax25_note_activity(void)
{
    m_last_activity = now_ms();
}

/* ------------------------------------------------------------- receive */

void m17_ax25_rx_packet(const unsigned char *data, int len)
{
    if (!m_on || data == NULL || len < 2)
        return;

    /* The wire test feeds a burst through the real receive chain; capture what
     * comes out instead of handing it to the KISS host, so the bytes can be
     * compared with what was sent. */
    if (m_wire_active) {
        int n = len - 1;
        if (n > (int)sizeof m_wire_got) n = (int)sizeof m_wire_got;
        memcpy(m_wire_got, data + 1, (size_t)n);
        m_wire_len = n;
        return;
    }

    /* data[0] is the M17 type specifier; KISS carries the frame without it.
     * Direction and station are spelled out: the console used to say only what
     * the frame was, so a burst you had just sent read exactly like one you had
     * heard. */
    printf("m17web: RX  AX.25 %d bytes  (heard over air)\n", len - 1);
    gui_log_event("rx", "RX  AX.25 %d bytes heard over air", len - 1);
    fflush(stdout);
    m_rx_frames++;
    kiss_write(data + 1, (size_t)(len - 1));
}

/* ------------------------------------------------- stream-mode reception */

/* Stream mode carries 16 bytes per frame with rate 1/2 convolutional coding,
 * interleaving, and the link setup frame repeated every six frames.  That is
 * what makes a voice over survive a link where a single packet-mode LSF does
 * not, so AX.25 frames ride in it:
 *
 *   A5 25 <len hi> <len lo> <AX.25 frame> <crc hi> <crc lo>
 *
 * The CRC covers the four header bytes and the frame.  The block is repeated
 * inside the burst, so a receiver that joins late still gets a whole copy. */
#define AX25_STREAM_MAGIC0 0xA5
#define AX25_STREAM_MAGIC1 0x25

void m17_ax25_set_framing(int stream)
{
    m_stream = stream ? 1 : 0;
}

int m17_ax25_framing(void)
{
    return m_stream;
}

int m17_ax25_is_stream_tag(const unsigned char *meta)
{
    return meta != NULL && meta[0] == 'A' && meta[1] == 'X'
        && meta[2] == '2' && meta[3] == '5';
}

void m17_ax25_rx_stream(const unsigned char *data, int len)
{
    static uint8_t buf[AX25_QUEUE_BYTES + 8];
    static size_t  have;

    if (!m_on || data == NULL || len <= 0)
        return;
    if (have + (size_t)len > sizeof buf)
        have = 0;
    memcpy(&buf[have], data, (size_t)len);
    have += (size_t)len;

    for (;;) {
        size_t p = 0;
        while (p + 4 <= have &&
               !(buf[p] == AX25_STREAM_MAGIC0 && buf[p + 1] == AX25_STREAM_MAGIC1))
            p++;
        if (p > 0) {
            memmove(buf, &buf[p], have - p);
            have -= p;
        }
        if (have < 6)
            return;

        size_t flen = ((size_t)buf[2] << 8) | buf[3];
        if (flen == 0 || flen + 6 > sizeof buf) {
            memmove(buf, &buf[2], have - 2);        /* bad length: rescan */
            have -= 2;
            continue;
        }
        if (have < flen + 6)
            return;                                  /* wait for more chunks */

        uint16_t crc = m17_crc_array_encode(buf, (int)(flen + 4));
        uint16_t got = ((uint16_t)buf[flen + 4] << 8) | buf[flen + 5];
        if (crc == got) {
            printf("m17web: RX  AX.25 %d bytes  (heard over air)\n", (int)flen);
    gui_log_event("rx", "RX  AX.25 %d bytes heard over air", (int)flen);
            fflush(stdout);
            m_rx_frames++;
            kiss_write(&buf[4], flen);
            memmove(buf, &buf[flen + 6], have - (flen + 6));
            have -= flen + 6;
        } else {
            memmove(buf, &buf[2], have - 2);
            have -= 2;
        }
    }
}

/* ------------------------------------------------------------ transmit */

/* Build the packet, then either loop it back or put it on the air. */
static void send_packet(const uint8_t *frame, size_t len)
{
    static uint8_t scratch[AX25_QUEUE_BYTES];
    uint8_t        meta[M17_META_BYTES];
    M17Type        type;

    if (len + 2 > sizeof scratch) {
        m_rejected++;
        return;
    }
    memcpy(scratch, frame, len);

    /* Link setup: broadcast destination, our own call as source, packet mode,
     * no encryption, CAN 0.  The radio's own builder produces the bytes; the
     * override in m_call is unused here because the LSF source is the station
     * address the rest of the program already uses. */
    memset(&type, 0, sizeof type);          /* p_s = 0: packet */
    memset(meta, 0, sizeof meta);

    if (m_loopback) {
        /* No radio: build the burst and reassemble it as the receiver would. */
        m17_pkt_burst    burst;
        m17_rx_assembler rx;
        m17_pkt_status   st = M17_PKT_NEED_MORE;

        size_t frames = m17_pkt_build(&burst, M17_TYPE_RAW, frame, len);
        if (frames > 0) {
            m17_rx_reset(&rx);
            for (size_t i = 0; i < frames; i++) {
                st = m17_rx_chunk(&rx, &burst.frame[i]);
                if (st != M17_PKT_NEED_MORE)
                    break;
            }
        }
        if (st == M17_PKT_COMPLETE && rx.len > 1) {
            m_looped++;
            printf("m17web: LB  AX.25 %d bytes  (loopback, no radio)\n", (int)rx.len - 1);
            gui_log_event("lb", "LB  AX.25 %d bytes loopback (no radio)", (int)rx.len - 1);
            fflush(stdout);
            kiss_write(&rx.data[1], rx.len - 1);
        } else {
            m_rejected++;
        }
        return;
    }

    printf("m17web: TX  AX.25 %d bytes  (this station, onto air, %d frame%s)\n",
           (int)len, (int)((len + 2 + 24) / 25), ((len + 2 + 24) / 25) == 1 ? "" : "s");
    gui_log_event("tx", "TX  AX.25 %d bytes onto air (%d frame%s)",
                  (int)len, (int)((len + 2 + 24) / 25), ((len + 2 + 24) / 25) == 1 ? "" : "s");
    fflush(stdout);

    static uint8_t blk[AX25_QUEUE_BYTES + 6];
    size_t n = 0;

    memset(&type, 0, sizeof type);
    memset(meta, 0, sizeof meta);

    if (m_stream) {
        /* Stream framing: our own header and CRC, tagged in the LSF meta. */
        blk[n++] = AX25_STREAM_MAGIC0;
        blk[n++] = AX25_STREAM_MAGIC1;
        blk[n++] = (uint8_t)((len >> 8) & 0xFF);
        blk[n++] = (uint8_t)(len & 0xFF);
        memcpy(&blk[n], scratch, len);
        n += len;
        uint16_t crc = m17_crc_array_encode(blk, (int)n);
        blk[n++] = (uint8_t)(crc >> 8);
        blk[n++] = (uint8_t)(crc & 0xFF);
        type.p_s = CCT_STREAM;
        type.dt  = DATA_DATA;           /* data, not voice */
        memcpy(meta, "AX25", 4);        /* marks the block as ours */
    } else {
        /* Packet framing, section 2.9: the payload starts with the protocol
         * specifier and m17_send_packet_frames() appends the CRC.  The type
         * word stays zero, which is P/S = 0, packet mode. */
        blk[n++] = 0x00;                /* RAW */
        memcpy(&blk[n], scratch, len);
        n += len;
    }

    radio_transmit();
    /* Silence either side of the burst.  The modulator is constant envelope, so
     * keying straight into a preamble and dropping the carrier straight after
     * the last frame are both step changes: audible as a click at each end and
     * splatter either side of the signal.  Zero samples give the DAC somewhere
     * to start and finish. */
    radio_silence(5);
    /* No blank carrier: a constant symbol stream correlates with any sync word,
     * so the receiver false-locks on it and decodes the silence as an LSF. */
    for (int i = 0; i < AX25_PREAMBLES; i++)
        m17_send_preamble();
    for (int i = 0; i < AX25_LSF_REPEATS; i++)
        m17_send_link_setup_frame(M17_ADDR_BROADCAST, m17_db_get_src(), type, meta);

    if (m_stream) {
        /* The LSF is carried five bytes at a time across six stream frames, so
         * the burst is padded with zeros to six frames: that completes the link
         * setup and flushes the decoder in one go. */
        size_t chunks = (n + 15) / 16;
        for (size_t i = 0; i < n; i += 16) {
            uint8_t chunk[16];
            size_t  take = n - i;
            memset(chunk, 0, sizeof chunk);
            if (take > 16)
                take = 16;
            memcpy(chunk, &blk[i], take);
            m17_send_stream_frame(chunk);
        }
        int tail = (chunks < 6) ? (int)(6 - chunks) : 2;
        for (int t = 0; t < tail; t++) {
            uint8_t zero[16];
            memset(zero, 0, sizeof zero);
            m17_send_stream_frame(zero);
        }
    } else {
        /* Packet mode: type byte and frame, chunked into 25-byte packet frames
         * with the CRC appended by the builder. */
        m17_send_packet_frames(blk, (int)n);
    }
    m17_send_eot();
    /* Hand over what the modulator and the radio's transmit accumulator are
     * still holding, and give the last chunk time to go out before the radio is
     * switched back to receive.  Without this the tail of the last frame in the
     * burst is never transmitted - which costs a packet its CRC. */
    radio_silence(20);
    m17_mod_flush();
#ifdef HAVE_PLUTO
    pluto_tx_flush();
    {   /* samples the radio refused: silent loss otherwise */
        static long seen;
        long now = pluto_tx_push_errors();
        if (now != seen) {
            printf("m17web: AX.25 transmit lost %ld sample buffers\n", now - seen);
            fflush(stdout);
            seen = now;
        }
    }
#endif
    usleep(60000);
    radio_receive();

    m_tx_frames++;
}

int m17_ax25_service(void)
{
    uint32_t now;
    size_t   len = 0;
    const uint8_t *frame;

    if (!m_on)
        return 0;

    now = now_ms();

    pthread_mutex_lock(&m_lock);
    int go = m17_bridge_tick(&m_bridge, now,
                             (now - m_last_activity) < AX25_DCD_MS,
                             m17_txrx_ptt_state() != 0,
                             now * 2654435761u);
    if (go)
        frame = m17_bridge_head(&m_bridge, &len);
    else
        frame = NULL;
    pthread_mutex_unlock(&m_lock);

    if (!go)
        return 0;

    if (frame != NULL)
        send_packet(frame, len);

    pthread_mutex_lock(&m_lock);
    m17_bridge_sent(&m_bridge, now_ms());
    pthread_mutex_unlock(&m_lock);
    return 1;
}

void m17_ax25_status(char *out, unsigned n)
{
    pthread_mutex_lock(&m_lock);
    snprintf(out, n,
             "AX.25 %s, loopback %s, port %u, queued %u, sent %u, "
             "rx %u, tx %u, looped %u, dropped %u (dup %u, full %u)",
             m_on ? "on" : "off", m_loopback ? "on" : "off", m_port,
             m_bridge.count, m_bridge.sent, m_rx_frames, m_tx_frames, m_looped,
             m_rejected, m_bridge.dropped_dup, m_bridge.dropped_full);
    pthread_mutex_unlock(&m_lock);
}
