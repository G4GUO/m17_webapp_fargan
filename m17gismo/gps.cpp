// gps.cpp
//
// GPS receiver support: read NMEA from a serial GPS, keep the current fix, and
// encode/decode the M17 LSF META GNSS position payload.
//
// The receiver is a u-blox 7 on USB, which appears as a CDC-ACM serial port
// (/dev/ttyACM<n>), not a ttyUSB bridge.  Its port number depends on what else
// enumerated first, so the device is resolved by identity:
//
//   1. the device set with "gps <device> [baud]" or M17WEB_GPS
//   2. the first u-blox / GPS device in /dev/serial/by-id
//   3. /dev/ttyACM0
//
// The port speed matters: this receiver only emits navigation sentences at
// 38400 baud (at 9600 it sends nothing but the u-blox banner text), so the port
// is configured with termios rather than left as the driver found it.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <time.h>
#include <glob.h>
#include <sys/stat.h>

#include "m17defines.h"

GpsMsg m_gps;

int nmea_decimal(const char *in, int n);           // defined below, used by the parsers
static void csv_tick(void);                        // defined below, called by the reader loop

static char        m_device[160];          // "" = resolve automatically
static int         m_baud = 38400;
static int         m_fd = -1;
static bool        m_gps_running;
static bool        m_thread_started;
static pthread_t   m_tid;
static char        m_status[128] = "not started";

static uint32_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

// ---------------------------------------------------------------------------
// Configuration and device resolution
// ---------------------------------------------------------------------------
void gps_set_device(const char *dev, int baud) {
    if (dev) snprintf(m_device, sizeof(m_device), "%s", dev);
    if (baud > 0) m_baud = baud;
}

const char *gps_device(void) { return m_device; }
int gps_baud(void) { return m_baud; }
const char *gps_status(void) { return m_status; }

// Baud rate constants are not contiguous, so they need a table.
static int baud_constant(int baud) {
    switch (baud) {
        case 4800:   return B4800;
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        default:     return B38400;
    }
}

// Case-insensitive substring test (avoid strcasestr/_GNU_SOURCE portability).
static bool ci_has(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return false;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return true;
    }
    return false;
}

// Anything that is definitely not a GPS receiver.  The STM32 modem's VCP lives
// on the same by-id tree, so this check is what stops us stealing it.
static bool device_is_forbidden(const char *path) {
    static const char *const bad[] = {
        "stlink", "st-link", "stm32", "pluto", "adal", "analog devices",
        "mass storage", "ttyama", "ttyacm0-pluto", NULL
    };
    for (int i = 0; bad[i]; i++) {
        if (ci_has(path, bad[i])) return true;
    }
    return false;
}

// Build the candidate list in priority order.  Explicit configuration first
// (operator knows best), then every by-id identity match.
static int collect_candidates(char list[][160], int max) {
    int n = 0;

    const char *explicit_dev = m_device[0] ? m_device : getenv("M17WEB_GPS");
    if (explicit_dev && explicit_dev[0] && n < max) {
        snprintf(list[n++], 160, "%s", explicit_dev);
    }

    static const char *const patterns[] = {
        "/dev/serial/by-id/*u-blox*",
        "/dev/serial/by-id/*ublox*",
        "/dev/serial/by-id/*GPS*",
        "/dev/serial/by-id/*GNSS*",
        NULL
    };
    for (int i = 0; patterns[i]; i++) {
        glob_t g;
        if (glob(patterns[i], 0, NULL, &g) == 0) {
            for (size_t k = 0; k < g.gl_pathc && n < max; k++) {
                const char *p = g.gl_pathv[k];
                if (device_is_forbidden(p)) continue;
                bool dup = false;
                for (int j = 0; j < n; j++) {
                    if (strcmp(list[j], p) == 0) { dup = true; break; }
                }
                if (!dup) snprintf(list[n++], 160, "%s", p);
            }
        }
        globfree(&g);
    }
    return n;
}

static int open_port(const char *dev);

// A real GPS talks NMEA unprompted.  Anything silent for a couple of seconds is
// not our receiver - including a modem VCP that accepted the open().
static bool probe_nmea(int fd, int ms) {
    uint32_t start = now_ms();
    char buf[256];
    int  dollars = 0;

    while (now_ms() - start < (uint32_t)ms) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == '$') {
                    if (++dollars >= 2) return true;
                } else if (dollars && buf[i] >= 'A' && buf[i] <= 'Z') {
                    return true;               // "$G", "$P" etc. - sentence start
                }
            }
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return false;
        }
    }
    return false;
}

// Resolve *and* validate: returns an open fd, or -1 with errno set.
static int open_gps(char *chosen, size_t chosen_len) {
    char cand[8][160];
    int n = collect_candidates(cand, 8);

    if (chosen_len) chosen[0] = 0;
    if (n <= 0) {
        errno = ENOENT;
        return -1;
    }

    for (int i = 0; i < n; i++) {
        if (device_is_forbidden(cand[i])) continue;

        int fd = open_port(cand[i]);
        if (fd < 0) continue;

        if (!probe_nmea(fd, 2500)) {
            close(fd);
            continue;
        }

        if (chosen_len) snprintf(chosen, chosen_len, "%s", cand[i]);
        // The probe above needs a non-blocking fd so it can give up at its own
        // deadline.  The reader loop wants the opposite: with O_NONBLOCK set the
        // kernel ignores VTIME, so read() returns EAGAIN the instant there is no
        // byte and the loop spins - a whole core burnt between the receiver's
        // 1 Hz sentences, which is exactly what it did on the Pi 5.  Clear it so
        // each read sleeps in the kernel until a byte arrives, or until VTIME
        // expires - and the loop already treats n == 0 as a read timeout.
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
        return fd;
    }

    errno = ENODEV;
    return -1;
}

static int open_port(const char *dev) {
    int fd = open(dev, O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;

    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) { close(fd); return -1; }
    cfmakeraw(&tio);
    cfsetispeed(&tio, baud_constant(m_baud));
    cfsetospeed(&tio, baud_constant(m_baud));
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 10;                      // 1 s read timeout
    if (tcsetattr(fd, TCSANOW, &tio) != 0) { close(fd); return -1; }
    tcflush(fd, TCIFLUSH);
    return fd;
}

// ---------------------------------------------------------------------------
// NMEA parsing
// ---------------------------------------------------------------------------
static bool nmea_chksum(const char *nmea) {
    char c = 0;
    const char *p = strchr(nmea, '*');
    if (!p) return false;
    for (const char *q = nmea + 1; q < p; q++) c ^= *q;
    return c == (char)strtol(p + 1, NULL, 16);
}

static int gps_tokenise(char *nmea, char *list[], int max) {
    int n = 0;
    char *p = nmea;
    if (n < max) list[n++] = p;
    for (; *p && n < max; p++) {
        if (*p == ',' || *p == '*') { *p = 0; if (n < max) list[n++] = p + 1; }
    }
    return n;
}

// "5048.23653" with 2 degree digits -> 50.803942
static double nmea_coord(const char *in, int deg_digits) {
    if (!in || !in[0]) return 0.0;
    char deg[4] = {0};
    for (int i = 0; i < deg_digits && in[i]; i++) deg[i] = in[i];
    double d = strtod(deg, NULL);
    double m = strtod(in + deg_digits, NULL);
    return d + m / 60.0;
}

static void gps_parse_nmea_locked(char *nmea);

void gps_parse_nmea(char *nmea) {
    if (!nmea || !nmea_chksum(nmea)) return;      // ignore corrupt lines
    gps_parse_nmea_locked(nmea);
}

static void gps_parse_nmea_locked(char *nmea) {
    char *list[30];
    int nt = gps_tokenise(nmea, list, 30);
    if (nt < 2) return;

    // GGA: fix quality, position, satellites, HDOP and altitude in metres
    if (strncmp(list[0], "$GPGGA", 6) == 0 || strncmp(list[0], "$GNGGA", 6) == 0) {
        if (nt < 10) return;
        int quality = atoi(list[6]);
        if (quality > 0 && list[2][0] && list[4][0]) {
            double lat = nmea_coord(list[2], 2);
            double lon = nmea_coord(list[4], 3);
            if (list[3][0] == 'S') lat = -lat;
            if (list[5][0] == 'W') lon = -lon;
            m_gps.lat = lat;
            m_gps.lon = lon;
            m_gps.valid = true;
            m_gps.fix_ms = now_ms();
            m_gps.nsats = (uint8_t)atoi(list[7]);
            m_gps.hdop = (float)strtod(list[8], NULL);
            if (list[9][0]) m_gps.alt = (int16_t)lround(strtod(list[9], NULL));  // metres
            m_gps.validity |= GNSS_VALID_POSITION | GNSS_VALID_ALTITUDE;
            if (m_gps.hdop > 0.0f) m_gps.validity |= GNSS_VALID_RADIUS;
        } else {
            m_gps.valid = false;
        }
        return;
    }

    // RMC: position, speed over ground (knots), track made good, date
    if (strncmp(list[0], "$GPRMC", 6) == 0 || strncmp(list[0], "$GNRMC", 6) == 0) {
        if (nt < 10) return;
        if (list[2][0] == 'A') {
            double lat = nmea_coord(list[3], 2);
            double lon = nmea_coord(list[5], 3);
            if (list[4][0] == 'S') lat = -lat;
            if (list[6][0] == 'W') lon = -lon;
            m_gps.lat = lat;
            m_gps.lon = lon;
            m_gps.valid = true;
            m_gps.fix_ms = now_ms();
            m_gps.validity |= GNSS_VALID_POSITION;
            if (list[7][0]) {
                // knots -> km/h, kept in 0.5 km/h steps by the META encoder
                m_gps.speed_kmh = strtod(list[7], NULL) * 1.852;
                m_gps.have_velocity = true;
                m_gps.validity |= GNSS_VALID_VELOCITY;
            }
            if (list[8][0]) m_gps.course = (uint16_t)lround(strtod(list[8], NULL));
        } else {
            m_gps.valid = false;
        }
        if (nt > 9 && list[9][0] && strlen(list[9]) >= 6) {
            m_gps.day   = nmea_decimal(list[9], 2);
            m_gps.month = nmea_decimal(list[9] + 2, 2);
            m_gps.year  = 2000 + nmea_decimal(list[9] + 4, 2);
        }
        if (nt > 1 && list[1][0] && strlen(list[1]) >= 6) {
            m_gps.hour = nmea_decimal(list[1], 2);
            m_gps.min  = nmea_decimal(list[1] + 2, 2);
            m_gps.sec  = nmea_decimal(list[1] + 4, 2);
        }
        return;
    }

    // VTG: speed and course directly in km/h
    if (strncmp(list[0], "$GPVTG", 6) == 0 || strncmp(list[0], "$GNVTG", 6) == 0) {
        if (nt < 8) return;
        if (list[1][0]) m_gps.course = (uint16_t)lround(strtod(list[1], NULL));
        if (list[7][0]) m_gps.speed_kmh = strtod(list[7], NULL);
        if (list[1][0] || list[7][0]) {
            m_gps.have_velocity = true;
            if (m_gps.valid) m_gps.validity |= GNSS_VALID_VELOCITY;
        }
        return;
    }

    // GSA: dilution of precision (fallback for HDOP)
    if (strncmp(list[0], "$GPGSA", 6) == 0 || strncmp(list[0], "$GNGSA", 6) == 0) {
        if (nt > 16 && m_gps.hdop <= 0.0f && list[16][0]) m_gps.hdop = (float)strtod(list[16], NULL);
        return;
    }

    // GLL: position and time (fallback, and the time many receivers send last)
    if (strncmp(list[0], "$GPGLL", 6) == 0 || strncmp(list[0], "$GNGLL", 6) == 0) {
        if (nt < 7) return;
        if (list[6][0] == 'A') {
            double lat = nmea_coord(list[1], 2);
            double lon = nmea_coord(list[3], 3);
            if (list[2][0] == 'S') lat = -lat;
            if (list[4][0] == 'W') lon = -lon;
            m_gps.lat = lat;
            m_gps.lon = lon;
            m_gps.valid = true;
            m_gps.fix_ms = now_ms();
            m_gps.validity |= GNSS_VALID_POSITION;
        } else {
            m_gps.valid = false;
        }
        if (list[5][0] && strlen(list[5]) >= 6) {
            m_gps.hour = nmea_decimal(list[5], 2);
            m_gps.min  = nmea_decimal(list[5] + 2, 2);
            m_gps.sec  = nmea_decimal(list[5] + 4, 2);
        }
        return;
    }
}

int nmea_decimal(const char *in, int n) {
    int v = 0;
    for (int i = 0; i < n && in[i]; i++) {
        if (in[i] < '0' || in[i] > '9') break;
        v *= 10;
        v += in[i] - '0';
    }
    return v;
}

// ---------------------------------------------------------------------------
// Reader thread: reconnects by itself, and exits on close
// ---------------------------------------------------------------------------
void *gps_thread(void *arg) {
    (void)arg;
    char line[512];
    int  used = 0;
    char dev[160];
    uint32_t last_byte = 0;

    while (m_gps_running) {
        m_fd = open_gps(dev, sizeof(dev));
        if (m_fd < 0) {
            snprintf(m_status, sizeof(m_status), "no GPS receiver found (%s), retrying", strerror(errno));
            // Try again: the receiver may not be plugged in yet.
            for (int i = 0; i < 20 && m_gps_running; i++) usleep(100000);
            continue;
        }
        snprintf(m_status, sizeof(m_status), "listening on %s at %d baud", dev, m_baud);
        printf("m17web: GPS on %s at %d baud\n", dev, m_baud);

        used = 0;
        last_byte = now_ms();
        while (m_gps_running) {
            char buf[256];
            ssize_t n = read(m_fd, buf, sizeof(buf));
            if (n > 0) last_byte = now_ms();
            if (n < 0) {
                // Second line of defence: if the fd is ever non-blocking again
                // (or a read returns EAGAIN for another reason), yield instead of
                // spinning on a whole core.
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) { usleep(2000); continue; }
                break;                                  // device went away
            }
            if (n == 0) {                               // read timeout
                csv_tick();
                // A live receiver emits a sentence every second.  Silence means we
                // are on the wrong device (or it is wedged): drop it and re-resolve.
                if (last_byte && (now_ms() - last_byte) > 30000U) break;
                continue;
            }
            for (ssize_t i = 0; i < n; i++) {
                char c = buf[i];
                if (c == '\n' || c == '\r') {
                    if (used > 6) {
                        line[used] = 0;
                        gps_parse_nmea(line);
                    }
                    used = 0;
                } else if (used < (int)sizeof(line) - 1) {
                    line[used++] = c;
                }
            }
            csv_tick();
        }
        close(m_fd);
        m_fd = -1;
        if (m_gps_running) {
            snprintf(m_status, sizeof(m_status), "%s went away, reopening", dev);
            usleep(500000);
        }
    }
    snprintf(m_status, sizeof(m_status), "stopped");
    return arg;
}

int gps_open(void) {
    if (m_thread_started) return 0;
    m_gps_running = true;
    m_gps.validity = 0;
    m_gps.station_type = GNSS_STATION_FIXED;   // a home station; see GNSS_STATION_* in m17defines.h
    if (pthread_create(&m_tid, NULL, &gps_thread, NULL) != 0) {
        printf("GPS thread cannot be started\n\r");
        m_gps_running = false;
        return -1;
    }
    m_thread_started = true;
    return 0;
}

void gps_close(void) {
    if (!m_thread_started) return;
    m_gps_running = false;
    pthread_join(m_tid, NULL);          // the read has a 1 s timeout, so this returns
    m_thread_started = false;
}

// ---------------------------------------------------------------------------
// Fix state for the rest of the program
// ---------------------------------------------------------------------------
bool gps_fix_fresh(int max_age_ms) {
    if (!m_gps.valid) return false;
    uint32_t age = now_ms() - m_gps.fix_ms;
    return age <= (uint32_t)max_age_ms;
}

uint32_t gps_fix_age_ms(void) {
    if (!m_gps.fix_ms) return 0;
    return now_ms() - m_gps.fix_ms;
}

double gps_lat(void) { return m_gps.lat; }
double gps_lon(void) { return m_gps.lon; }

void gps_status_text(char *out, int len) {
    if (!m_gps.valid) { snprintf(out, len, "no fix"); return; }
    char grid[12];
    gps_maidenhead(grid, sizeof(grid));
    snprintf(out, len, "%.5f, %.5f %s %dsat%s%s", m_gps.lat, m_gps.lon, grid, m_gps.nsats,
             m_gps.hdop > 0.0f ? "" : "", "");
}

// ---------------------------------------------------------------------------
// Maidenhead locator
// ---------------------------------------------------------------------------
void gps_maidenhead_of(double lat, double lon, char *out, int len) {
    if (len < 7) { if (len > 0) out[0] = 0; return; }
    if (lon < -180.0) lon = -180.0;
    if (lon >  180.0) lon =  180.0;
    if (lat <  -90.0) lat =  -90.0;
    if (lat >   90.0) lat =   90.0;

    double lo = lon + 180.0;                 // 0..360
    double la = lat +  90.0;                 // 0..180

    int f_lo = (int)(lo / 20.0);             // field, A..R
    int f_la = (int)(la / 10.0);
    lo -= f_lo * 20.0;
    la -= f_la * 10.0;

    int s_lo = (int)(lo / 2.0);              // square, 0..9
    int s_la = (int)(la / 1.0);
    lo -= s_lo * 2.0;
    la -= s_la * 1.0;

    int t_lo = (int)(lo / (2.0 / 24.0));     // subsquare, A..X
    int t_la = (int)(la / (1.0 / 24.0));

    if (f_lo > 17) f_lo = 17; if (f_la > 17) f_la = 17;
    if (s_lo > 9)  s_lo = 9;  if (s_la > 9)  s_la = 9;
    if (t_lo > 23) t_lo = 23; if (t_la > 23) t_la = 23;

    char buf[8];
    buf[0] = (char)('A' + f_lo);
    buf[1] = (char)('A' + f_la);
    buf[2] = (char)('0' + s_lo);
    buf[3] = (char)('0' + s_la);
    buf[4] = (char)('A' + t_lo);
    buf[5] = (char)('A' + t_la);
    buf[6] = 0;

    int n = (len < 7) ? len : 7;      // six characters plus the terminator
    snprintf(out, n, "%s", buf);
}

void gps_maidenhead(char *out, int len) {
    if (!m_gps.valid) { if (len > 0) snprintf(out, len, "--"); return; }
    gps_maidenhead_of(m_gps.lat, m_gps.lon, out, len);
}

// ---------------------------------------------------------------------------
// M17 LSF META "GNSS Position Data" (encryption subtype 01), 14 bytes / 112 bits
//
// Layout as of specification 2.0.9 - values are metric, and the order is:
//   byte 0      Data Source (4) | Station Type (4)
//   byte 1      Validity (4) | Radius (3) | Bearing bit 8 (1)
//   byte 2      Bearing bits 7..0
//   bytes 3..5  Latitude,  24 bit two's complement, +-8388607 = +-90 degrees
//   bytes 6..8  Longitude, 24 bit two's complement, +-8388607 = +-180 degrees
//   bytes 9..10 Altitude,  16 bit, 0.5 m steps, offset 500 m
//   byte 11     Speed bits 11..4
//   byte 12     Speed bits 3..0 | Reserved (4)
//   byte 13     Reserved
//
// Bit packing is done with a small MSB-first writer rather than shifts, because
// the fields do not sit on byte boundaries.
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t *b;
    int bit;
} BitWriter;

static void bw_put(BitWriter *w, uint32_t value, int bits) {
    for (int i = bits - 1; i >= 0; i--) {
        int byte = w->bit >> 3;
        int mask = 0x80 >> (w->bit & 7);
        if ((value >> i) & 1) w->b[byte] |= mask;
        else                  w->b[byte] &= (uint8_t)~mask;
        w->bit++;
    }
}

static uint32_t bw_get(const uint8_t *b, int *bit, int bits) {
    uint32_t v = 0;
    for (int i = 0; i < bits; i++) {
        int byte = *bit >> 3;
        int mask = 0x80 >> (*bit & 7);
        v = (v << 1) | ((b[byte] & mask) ? 1u : 0u);
        (*bit)++;
    }
    return v;
}

// The specification describes the radius as an HDOP-derived lateral uncertainty
// but does not define a mapping to the 3 bit field, so this is a documented
// approximation: bands of uncertainty in metres, 0 = unknown.
static uint32_t radius_code(float hdop) {
    if (hdop <= 0.0f) return 0;
    double m = hdop * 5.0;                       // conservative: ~5 m per HDOP unit
    if (m <   2.0) return 1;
    if (m <   5.0) return 2;
    if (m <  10.0) return 3;
    if (m <  25.0) return 4;
    if (m <  50.0) return 5;
    if (m < 100.0) return 6;
    return 7;
}

void gps_encode(uint8_t *b, GpsMsg *msg) {
    memset(b, 0, 14);

    BitWriter w = { b, 0 };
    bw_put(&w, GNSS_SOURCE_M17_CLIENT, 4);
    bw_put(&w, msg->station_type, 4);

    bw_put(&w, msg->validity, 4);
    bw_put(&w, radius_code(msg->hdop), 3);

    uint32_t bearing = msg->course % 360;
    bw_put(&w, (bearing >> 8) & 0x01, 1);        // most significant bit
    bw_put(&w, bearing & 0xFF, 8);

    long lat = lround(msg->lat / 90.0  * 8388607.0);
    long lon = lround(msg->lon / 180.0 * 8388607.0);
    if (lat >  8388607) lat =  8388607;
    if (lat < -8388607) lat = -8388607;
    if (lon >  8388607) lon =  8388607;
    if (lon < -8388607) lon = -8388607;
    bw_put(&w, (uint32_t)lat & 0xFFFFFF, 24);
    bw_put(&w, (uint32_t)lon & 0xFFFFFF, 24);

    long alt = lround((msg->alt + 500.0) / 0.5);
    if (alt < 0) alt = 0;
    if (alt > 65535) alt = 65535;
    bw_put(&w, (uint32_t)alt, 16);

    long spd = lround(msg->speed_kmh / 0.5);
    if (spd < 0) spd = 0;
    if (spd > 4095) spd = 4095;
    bw_put(&w, (uint32_t)spd, 12);
    bw_put(&w, 0, 4);                            // reserved
    bw_put(&w, 0, 8);                            // reserved
}

void gps_decode(uint8_t *b, GpsMsg *msg) {
    int bit = 0;
    uint32_t source = bw_get(b, &bit, 4);
    uint32_t station = bw_get(b, &bit, 4);
    uint32_t validity = bw_get(b, &bit, 4);
    uint32_t radius = bw_get(b, &bit, 3);
    uint32_t bearing = (bw_get(b, &bit, 1) << 8);
    bearing |= bw_get(b, &bit, 8);
    uint32_t lat = bw_get(b, &bit, 24);
    uint32_t lon = bw_get(b, &bit, 24);
    uint32_t alt = bw_get(b, &bit, 16);
    uint32_t spd = bw_get(b, &bit, 12);

    (void)source;
    (void)radius;
    msg->station_type = (uint8_t)station;
    msg->validity     = (uint8_t)validity;
    msg->course       = (uint16_t)bearing;
    msg->speed_kmh    = spd * 0.5;
    msg->alt          = (int16_t)lround(alt * 0.5 - 500.0);
    msg->lat          = (double)(int32_t)(lat << 8) / 256.0 / 8388607.0 * 90.0;
    msg->lon          = (double)(int32_t)(lon << 8) / 256.0 / 8388607.0 * 180.0;
    msg->valid        = (validity & GNSS_VALID_POSITION) ? true : false;
}

// ---------------------------------------------------------------------------
// Where a station is relative to us: initial great circle bearing and distance.
// The compass in the web interface shows the bearing to the station being heard,
// which is the number that is useful for pointing an antenna.
// ---------------------------------------------------------------------------
static double deg2rad_(double d) { return d * M_PI / 180.0; }
static double rad2deg_(double r) { return r * 180.0 / M_PI; }

double gps_bearing(double lat1, double lon1, double lat2, double lon2) {
    double p1 = deg2rad_(lat1), p2 = deg2rad_(lat2), dl = deg2rad_(lon2 - lon1);
    double y = sin(dl) * cos(p2);
    double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    double b = rad2deg_(atan2(y, x));
    if (b < 0.0) b += 360.0;
    return b;
}

double gps_distance_km(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371.0088;                     // mean Earth radius
    double p1 = deg2rad_(lat1), p2 = deg2rad_(lat2);
    double dp = p2 - p1, dl = deg2rad_(lon2 - lon1);
    double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    if (a < 0.0) a = 0.0;
    if (a > 1.0) a = 1.0;
    return 2.0 * R * asin(sqrt(a));
}

// ---------------------------------------------------------------------------
// Position log: one CSV row every few seconds while it is switched on.
// ---------------------------------------------------------------------------
static bool     m_csv_on;
static int      m_csv_interval = 5;                 // seconds between rows
static char     m_csv_path[256];
static uint32_t m_csv_rows;
static uint32_t m_csv_last_ms;
static FILE    *m_csv_fp;

void gps_csv_set(bool on, int secs) {
    if (secs > 0) m_csv_interval = secs;
    if (m_csv_interval < 1) m_csv_interval = 1;
    if (on == m_csv_on) return;
    m_csv_on = on;
    if (!on) {
        if (m_csv_fp) { fclose(m_csv_fp); m_csv_fp = NULL; }
        return;
    }
    // Start the next row from now rather than immediately repeating one
    m_csv_last_ms = 0;
    printf("m17web: position log %s every %d s\n", m_csv_path, m_csv_interval);
}

bool gps_csv_on(void) { return m_csv_on; }
int  gps_csv_rows(void) { return (int)m_csv_rows; }
int  gps_csv_interval(void) { return m_csv_interval; }
const char *gps_csv_path(void) { return m_csv_path; }

static void csv_open(void) {
    if (!m_csv_path[0]) {
        const char *env = getenv("M17WEB_CSV");
        snprintf(m_csv_path, sizeof(m_csv_path), "%s",
                 (env && env[0]) ? env : "gps_track.csv");
    }
    bool fresh = access(m_csv_path, F_OK) != 0;
    m_csv_fp = fopen(m_csv_path, "a");
    if (!m_csv_fp) {
        printf("m17web: cannot open the position log %s\n", m_csv_path);
        return;
    }
    if (fresh) {
        fprintf(m_csv_fp, "date,time,lat,lon,locator,alt_m,speed_kmh,course_deg,satellites,hdop,fix_age_s,epoch\n");
        fflush(m_csv_fp);
    }
}

// Called from the reader loop, which wakes at least once a second.
static void csv_tick(void) {
    if (!m_csv_on || !m_gps.valid) return;
    uint32_t now = now_ms();
    if (m_csv_last_ms && (now - m_csv_last_ms) < (uint32_t)(m_csv_interval * 1000)) return;
    m_csv_last_ms = now;
    if (!m_csv_fp) {
        csv_open();
        if (!m_csv_fp) return;
    }
    char grid[12];
    gps_maidenhead(grid, sizeof(grid));
    // The GPS date and time are only as fresh as the last sentence received, so
    // an epoch column is written too: it is unambiguous and sorts correctly.
    fprintf(m_csv_fp, "%04d-%02d-%02d,%02d:%02d:%02d,%.6f,%.6f,%s,%d,%.1f,%d,%d,%.2f,%.1f,%ld\n",
            m_gps.year, m_gps.month, m_gps.day, m_gps.hour, m_gps.min, m_gps.sec,
            m_gps.lat, m_gps.lon, grid, (int)m_gps.alt, m_gps.speed_kmh,
            (int)m_gps.course, (int)m_gps.nsats, m_gps.hdop,
            (now - m_gps.fix_ms) / 1000.0, (long)time(NULL));
    fflush(m_csv_fp);
    if (ferror(m_csv_fp)) {                 // the mount went away: retry later
        fclose(m_csv_fp);
        m_csv_fp = NULL;
        return;
    }
    m_csv_rows++;
}

// One line describing a received position, for the log and the web interface.
void gps_format_position(double lat, double lon, char *out, int len) {
    char grid[12];
    gps_maidenhead_of(lat, lon, grid, sizeof(grid));
    snprintf(out, len, "%.5f, %.5f (%s)", lat, lon, grid);
}
