// webserver.c
//
// A small, dependency-free HTTP + WebSocket server used by the m17web
// browser front end.  Single event-loop thread; callbacks are invoked from
// that thread (so they must not block).  Sending is thread safe so the TX/RX
// threads can push audio and log lines at any time.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "webserver.h"

#define MAX_CLIENTS      8
#define MAX_HTTP_HEADER  (16*1024)
#define MAX_WS_MSG       (1*1024*1024)
#define MAX_OUT_QUEUE    (2*1024*1024)
#define CLIENT_TIMEOUT_MS 20000

// ---------------------------------------------------------------------------
// SHA1 (for the WebSocket handshake)
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t h[5];
    uint64_t len;
    unsigned char buf[64];
    int n;
} sha1_t;

static void sha1_block(sha1_t *s, const unsigned char *p) {
    uint32_t w[80], a, b, c, d, e, t;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
    for (int i = 16; i < 80; i++) {
        w[i] = w[i-3]^w[i-8]^w[i-14]^w[i-16];
        w[i] = (w[i]<<1)|(w[i]>>31);
    }
    a=s->h[0]; b=s->h[1]; c=s->h[2]; d=s->h[3]; e=s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b&c)|((~b)&d);        k = 0x5A827999; }
        else if (i < 40) { f = b^c^d;                 k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b&c)|(b&d)|(c&d);     k = 0x8F1BBCDC; }
        else             { f = b^c^d;                 k = 0xCA62C1D6; }
        t = ((a<<5)|(a>>27)) + f + e + k + w[i];
        e = d; d = c; c = (b<<30)|(b>>2); b = a; a = t;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d; s->h[4]+=e;
}

static void sha1_init(sha1_t *s) {
    s->h[0]=0x67452301; s->h[1]=0xEFCDAB89; s->h[2]=0x98BADCFE;
    s->h[3]=0x10325476; s->h[4]=0xC3D2E1F0;
    s->len = 0; s->n = 0;
}

static void sha1_update(sha1_t *s, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    s->len += len;
    while (len) {
        size_t k = 64 - s->n;
        if (k > len) k = len;
        memcpy(&s->buf[s->n], p, k);
        s->n += (int)k; p += k; len -= k;
        if (s->n == 64) { sha1_block(s, s->buf); s->n = 0; }
    }
}

static void sha1_final(sha1_t *s, unsigned char out[20]) {
    uint64_t bits = s->len * 8;
    unsigned char pad = 0x80;
    sha1_update(s, &pad, 1);
    unsigned char z = 0;
    while (s->n != 56) sha1_update(s, &z, 1);
    unsigned char l[8];
    for (int i = 0; i < 8; i++) l[i] = (unsigned char)(bits >> (56 - i*8));
    sha1_update(s, l, 8);
    for (int i = 0; i < 5; i++) {
        out[i*4]   = (unsigned char)(s->h[i] >> 24);
        out[i*4+1] = (unsigned char)(s->h[i] >> 16);
        out[i*4+2] = (unsigned char)(s->h[i] >> 8);
        out[i*4+3] = (unsigned char)(s->h[i]);
    }
}

static void base64_encode(const unsigned char *in, int len, char *out) {
    static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i = 0, o = 0;
    while (i < len) {
        int r = len - i;
        uint32_t v = in[i] << 16;
        if (r > 1) v |= in[i+1] << 8;
        if (r > 2) v |= in[i+2];
        out[o++] = t[(v >> 18) & 0x3F];
        out[o++] = t[(v >> 12) & 0x3F];
        out[o++] = (r > 1) ? t[(v >> 6) & 0x3F] : '=';
        out[o++] = (r > 2) ? t[v & 0x3F] : '=';
        i += 3;
    }
    out[o] = 0;
}

// ---------------------------------------------------------------------------
// Client / server state
// ---------------------------------------------------------------------------
typedef struct outbuf {
    struct outbuf *next;
    unsigned char *data;
    int len, off;
} outbuf;

typedef struct {
    int   fd;
    int   id;
    char  peer[64];
    char  role[16];          // ?client=phone; "" for the page
    int   is_ws;
    int   dead;
    long  last_ms;

    unsigned char *rx;   int rx_len, rx_cap;      // HTTP header / frame bytes
    int   frag_op;                                // 0 none, 1 text, 2 binary
    unsigned char *frag; int frag_len, frag_cap;

    outbuf *head, *tail;
    int   qbytes;
} client;

typedef struct {
    int         listen_fd;
    int         port;
    pthread_t   tid;
    int         running;
    int         started;
    int         next_id;
    client      clients[MAX_CLIENTS];
    pthread_mutex_t mtx;        // guards out queues
    WsConfig    cfg;
} server;

static server g_srv;
static void *g_handle = &g_srv;

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

static int set_nonblock(int fd) {
    int f = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

// ---------------------------------------------------------------------------
// Output queue
// ---------------------------------------------------------------------------
static void client_queue_flush(client *c) {
    // caller holds g_srv.mtx
    while (c->head) {
        outbuf *b = c->head;
        ssize_t n = send(c->fd, b->data + b->off, b->len - b->off, MSG_NOSIGNAL);
        if (n > 0) {
            b->off += (int)n;
            c->qbytes -= (int)n;
            if (b->off >= b->len) {
                c->head = b->next;
                if (!c->head) c->tail = NULL;
                free(b->data); free(b);
            }
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        } else {
            c->dead = 1;
            return;
        }
    }
}

static void client_queue_free(client *c) {
    outbuf *b = c->head;
    while (b) {
        outbuf *n = b->next;
        free(b->data); free(b);
        b = n;
    }
    c->head = c->tail = NULL;
    c->qbytes = 0;
}

static int client_send(int id, int opcode, const unsigned char *data, int len) {
    if (id < 0 || id >= MAX_CLIENTS) return -1;
    client *c = &g_srv.clients[id];
    if (c->dead || !c->is_ws || c->fd < 0) return -1;

    int hlen = (len < 126) ? 2 : (len < 65536 ? 4 : 10);
    outbuf *b = (outbuf *)malloc(sizeof(outbuf));
    if (!b) return -1;
    b->data = (unsigned char *)malloc(hlen + len);
    if (!b->data) { free(b); return -1; }
    b->len = hlen + len; b->off = 0; b->next = NULL;

    b->data[0] = (unsigned char)(0x80 | opcode);
    if (len < 126) {
        b->data[1] = (unsigned char)len;
    } else if (len < 65536) {
        b->data[1] = 126;
        b->data[2] = (unsigned char)(len >> 8);
        b->data[3] = (unsigned char)(len & 0xFF);
    } else {
        b->data[1] = 127;
        memset(&b->data[2], 0, 4);
        b->data[6] = (unsigned char)(len >> 24);
        b->data[7] = (unsigned char)(len >> 16);
        b->data[8] = (unsigned char)(len >> 8);
        b->data[9] = (unsigned char)(len & 0xFF);
    }
    if (len > 0) memcpy(b->data + hlen, data, len);

    pthread_mutex_lock(&g_srv.mtx);
    if (c->dead) {
        pthread_mutex_unlock(&g_srv.mtx);
        free(b->data); free(b);
        return -1;
    }
    if (c->qbytes > MAX_OUT_QUEUE) {
        // Client is not keeping up: drop this message rather than stall the radio
        pthread_mutex_unlock(&g_srv.mtx);
        free(b->data); free(b);
        return -1;
    }
    if (c->tail) c->tail->next = b; else c->head = b;
    c->tail = b;
    c->qbytes += b->len;
    client_queue_flush(c);
    pthread_mutex_unlock(&g_srv.mtx);
    return 0;
}

int ws_send_text(void *h, int id, const char *text) {
    (void)h;
    if (!text) return -1;
    return client_send(id, 0x1, (const unsigned char *)text, (int)strlen(text));
}

int ws_send_bin(void *h, int id, const uint8_t *data, int len) {
    (void)h;
    return client_send(id, 0x2, data, len);
}

int ws_broadcast_text(void *h, const char *text) {
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_srv.clients[i].is_ws && !g_srv.clients[i].dead && g_srv.clients[i].fd >= 0)
            if (ws_send_text(h, i, text) == 0) n++;
    return n;
}

int ws_broadcast_bin(void *h, const uint8_t *data, int len) {
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_srv.clients[i].is_ws && !g_srv.clients[i].dead && g_srv.clients[i].fd >= 0)
            if (ws_send_bin(h, i, data, len) == 0) n++;
    return n;
}

int ws_broadcast_bin_role(void *h, const char *role, const uint8_t *data, int len) {
    int n = 0;
    if (!role) return ws_broadcast_bin(h, data, len);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client *c = &g_srv.clients[i];
        if (c->is_ws && !c->dead && c->fd >= 0 && strcmp(c->role, role) == 0)
            if (ws_send_bin(h, i, data, len) == 0) n++;
    }
    return n;
}

const char *ws_client_role(void *h, int id) {
    (void)h;
    if (id < 0 || id >= MAX_CLIENTS) return "";
    return g_srv.clients[id].role;
}

int ws_count_role(void *h, const char *role) {
    (void)h;
    int n = 0;
    if (!role) return 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client *c = &g_srv.clients[i];
        if (c->is_ws && !c->dead && c->fd >= 0 && strcmp(c->role, role) == 0) n++;
    }
    return n;
}

int ws_client_count(void *h) {
    (void)h;
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_srv.clients[i].is_ws && !g_srv.clients[i].dead && g_srv.clients[i].fd >= 0) n++;
    return n;
}

int ws_list_clients(void *h, WsClientInfo *out, int max) {
    (void)h;
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS && n < max; i++) {
        client *c = &g_srv.clients[i];
        if (c->is_ws && !c->dead && c->fd >= 0) {
            out[n].id = c->id;
            snprintf(out[n].peer, sizeof(out[n].peer), "%s", c->peer);
            snprintf(out[n].role, sizeof(out[n].role), "%s", c->role);
            n++;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// Client lifecycle
// ---------------------------------------------------------------------------
static void client_drop(client *c, int notify) {
    int id = c->id;
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    pthread_mutex_lock(&g_srv.mtx);
    client_queue_free(c);
    pthread_mutex_unlock(&g_srv.mtx);
    free(c->rx);  c->rx = NULL;  c->rx_len = c->rx_cap = 0;
    free(c->frag); c->frag = NULL; c->frag_len = c->frag_cap = 0; c->frag_op = 0;
    int was_ws = c->is_ws;
    c->is_ws = 0;
    c->dead = 0;
    if (notify && was_ws && g_srv.cfg.on_close) g_srv.cfg.on_close(id, g_srv.cfg.user);
}

static int rx_reserve(client *c, int need) {
    if (c->rx_cap >= need) return 0;
    int cap = c->rx_cap ? c->rx_cap : 4096;
    while (cap < need) cap *= 2;
    unsigned char *p = (unsigned char *)realloc(c->rx, cap);
    if (!p) return -1;
    c->rx = p; c->rx_cap = cap;
    return 0;
}

// ---------------------------------------------------------------------------
// Static files
// ---------------------------------------------------------------------------
typedef struct { const char *ext; const char *mime; } mime_t;

static const mime_t mimes[] = {
    { ".html", "text/html; charset=utf-8" },
    { ".htm",  "text/html; charset=utf-8" },
    { ".js",   "application/javascript; charset=utf-8" },
    { ".mjs",  "application/javascript; charset=utf-8" },
    { ".css",  "text/css; charset=utf-8" },
    { ".json", "application/json; charset=utf-8" },
    { ".svg",  "image/svg+xml" },
    { ".png",  "image/png" },
    { ".ico",  "image/x-icon" },
    { ".webmanifest", "application/manifest+json" },
    { ".wav",  "audio/wav" },
    { ".txt",  "text/plain; charset=utf-8" },
    { NULL, NULL }
};

static const char *mime_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (dot) for (int i = 0; mimes[i].ext; i++)
        if (strcasecmp(dot, mimes[i].ext) == 0) return mimes[i].mime;
    return "application/octet-stream";
}

static void http_send(client *c, int code, const char *mime, const unsigned char *body, int len) {
    char hdr[512];
    const char *status = (code == 200) ? "200 OK" : (code == 404 ? "404 Not Found" : "400 Bad Request");
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %d\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     status, mime, len);
    // Blocking write for the (small, local) static response
    int fl = fcntl(c->fd, F_GETFL, 0);
    fcntl(c->fd, F_SETFL, fl & ~O_NONBLOCK);
    if (write(c->fd, hdr, n) < 0) { /* client gone */ }
    if (body && len > 0) {
        int off = 0;
        while (off < len) {
            ssize_t w = write(c->fd, body + off, len - off);
            if (w <= 0) break;
            off += (int)w;
        }
    }
    fcntl(c->fd, F_SETFL, fl);
}

static void http_serve_path(client *c, const char *path_in, const WsConfig *cfg) {
    char path[512];
    snprintf(path, sizeof(path), "%s", path_in);
    char *q = strchr(path, '?');
    if (q) *q = 0;
    if (path[0] == 0) snprintf(path, sizeof(path), "/index.html");
    if (path[strlen(path)-1] == '/') strncat(path, "index.html", sizeof(path) - strlen(path) - 1);
    if (strstr(path, "..")) { http_send(c, 404, "text/plain", (const unsigned char *)"bad path\n", 9); return; }

    // 1) the file system
    if (cfg->www_dir && cfg->www_dir[0]) {
        char full[1024];
        snprintf(full, sizeof(full), "%s%s", cfg->www_dir, path);
        FILE *f = fopen(full, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz >= 0 && sz < 4*1024*1024) {
                unsigned char *buf = (unsigned char *)malloc(sz ? sz : 1);
                if (buf) {
                    size_t got = fread(buf, 1, (size_t)sz, f);
                    fclose(f);
                    http_send(c, 200, mime_for(path), buf, (int)got);
                    free(buf);
                    return;
                }
            }
            fclose(f);
        }
    }
    // 2) embedded assets (built with www_assets.h)
    if (cfg->assets) {
        for (int i = 0; cfg->assets[i].path; i++) {
            if (strcmp(cfg->assets[i].path, path) == 0) {
                http_send(c, 200, cfg->assets[i].mime, cfg->assets[i].data, (int)cfg->assets[i].len);
                return;
            }
        }
    }
    const char *msg = "404 not found\n";
    // Helpful for the default case: point at the UI
    if (strcmp(path, "/index.html") == 0) msg = "m17web: no www assets found (build with www/ or pass -www dir)\n";
    http_send(c, 404, "text/plain; charset=utf-8", (const unsigned char *)msg, (int)strlen(msg));
}

// ---------------------------------------------------------------------------
// WebSocket
// ---------------------------------------------------------------------------
static int ws_handshake(client *c, const char *key) {
    char accept_in[256];
    unsigned char digest[20];
    char b64[64];
    sha1_t s;

    snprintf(accept_in, sizeof(accept_in), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    sha1_init(&s);
    sha1_update(&s, accept_in, strlen(accept_in));
    sha1_final(&s, digest);
    base64_encode(digest, 20, b64);

    char resp[512];
    int n = snprintf(resp, sizeof(resp),
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n", b64);
    int fl = fcntl(c->fd, F_GETFL, 0);
    fcntl(c->fd, F_SETFL, fl & ~O_NONBLOCK);
    ssize_t w = write(c->fd, resp, n);
    fcntl(c->fd, F_SETFL, fl);
    return (w == n) ? 0 : -1;
}

static int frag_append(client *c, const unsigned char *p, int len) {
    if (c->frag_len + len > MAX_WS_MSG) return -1;
    if (c->frag_cap < c->frag_len + len) {
        int cap = c->frag_cap ? c->frag_cap : 4096;
        while (cap < c->frag_len + len) cap *= 2;
        unsigned char *q = (unsigned char *)realloc(c->frag, cap);
        if (!q) return -1;
        c->frag = q; c->frag_cap = cap;
    }
    memcpy(c->frag + c->frag_len, p, len);
    c->frag_len += len;
    return 0;
}

static void ws_handle_frame(client *c, const unsigned char *b, int len) {
    if (len < 2) return;
    int fin    = (b[0] & 0x80) ? 1 : 0;
    int opcode = b[0] & 0x0F;
    int masked = (b[1] & 0x80) ? 1 : 0;
    uint64_t plen = b[1] & 0x7F;
    int pos = 2;

    if (plen == 126) {
        if (len < 4) return;
        plen = ((uint64_t)b[2] << 8) | b[3];
        pos = 4;
    } else if (plen == 127) {
        if (len < 10) return;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | b[2+i];
        pos = 10;
    }
    if (!masked) { c->dead = 1; return; }         // clients must mask
    if (plen > MAX_WS_MSG) { c->dead = 1; return; }
    if ((uint64_t)(len - pos) < 4 + plen) return; // not all here yet (shouldn't happen)

    unsigned char mask[4];
    memcpy(mask, b + pos, 4);
    pos += 4;

    unsigned char *payload = (unsigned char *)(b + pos);
    for (uint64_t i = 0; i < plen; i++) payload[i] ^= mask[i & 3];

    if (opcode == 0x8) {                          // close
        unsigned char echo[2] = { 0x88, 0x00 };
        send(c->fd, echo, 2, MSG_NOSIGNAL);
        c->dead = 1;
    } else if (opcode == 0x9) {                   // ping
        client_send(c->id, 0xA, payload, (int)plen);
    } else if (opcode == 0xA) {                   // pong
        // ignore
    } else if (opcode == 0x1 || opcode == 0x2) {  // text / binary
        int type = (opcode == 0x2) ? 2 : 1;
        if (fin) {
            if (g_srv.cfg.on_msg) g_srv.cfg.on_msg(c->id, type == 2, payload, (int)plen, g_srv.cfg.user);
        } else {
            c->frag_op = type;
            c->frag_len = 0;
            if (frag_append(c, payload, (int)plen) < 0) { c->dead = 1; }
        }
    } else if (opcode == 0x0) {                   // continuation
        if (!c->frag_op) { c->dead = 1; return; }
        if (frag_append(c, payload, (int)plen) < 0) { c->dead = 1; return; }
        if (fin) {
            if (g_srv.cfg.on_msg) g_srv.cfg.on_msg(c->id, c->frag_op == 2, c->frag, c->frag_len, g_srv.cfg.user);
            c->frag_op = 0; c->frag_len = 0;
        }
    }
}

// Serve bytes received from a client: works out whether we have a complete
// HTTP request or one/more WebSocket frames.
static void client_process(client *c) {
    for (;;) {
        if (!c->is_ws) {
            // Wait for the end of the HTTP header block
            unsigned char *end = NULL;
            for (int i = 0; i + 3 < c->rx_len; i++) {
                if (c->rx[i] == '\r' && c->rx[i+1] == '\n' && c->rx[i+2] == '\r' && c->rx[i+3] == '\n') {
                    end = c->rx + i; break;
                }
            }
            if (!end) {
                if (c->rx_len > MAX_HTTP_HEADER) c->dead = 1;
                return;
            }
            int hlen = (int)(end - c->rx) + 4;
            char req[2048];
            int n = hlen < (int)sizeof(req) ? hlen : (int)sizeof(req) - 1;
            memcpy(req, c->rx, n);
            req[n] = 0;

            char method[16] = {0}, path[1024] = {0};
            if (sscanf(req, "%15s %1023s", method, path) != 2) { http_send(c, 400, "text/plain", (const unsigned char *)"bad request\n", 12); c->dead = 1; return; }

            // Case-insensitive look for the upgrade/key headers
            int is_upgrade = 0;
            char key[128] = {0};
            char lower[2048];
            for (int i = 0; i < n; i++) lower[i] = (char)tolower((unsigned char)req[i]);
            lower[n] = 0;
            if (strstr(lower, "upgrade: websocket")) is_upgrade = 1;
            const char *kp = strstr(lower, "sec-websocket-key:");
            if (kp) {
                kp += strlen("sec-websocket-key:");
                while (*kp == ' ') kp++;
                const char *ke = strstr(kp, "\r\n");
                int klen = ke ? (int)(ke - kp) : 0;
                if (klen > 0 && klen < (int)sizeof(key)) {
                    // copy from the original (case preserved)
                    int off = (int)(kp - lower);
                    memcpy(key, req + off, klen);
                    key[klen] = 0;
                }
            }

            if (is_upgrade && key[0] && strcmp(method, "GET") == 0) {
                // Which client is this?  The phone app asks for /ws?client=phone;
                // anything else (the page) is the default role.
                c->role[0] = 0;
                {
                    const char *q = strchr(path, '?');
                    while (q) {
                        q++;
                        if (strncmp(q, "client=", 7) == 0) {
                            int k = 0;
                            q += 7;
                            while (*q && *q != '&' && k < (int)sizeof(c->role) - 1) {
                                char ch = *q++;
                                if (ch == '%') break;          // no need for escapes here
                                if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                                    (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')
                                    c->role[k++] = (char)tolower((unsigned char)ch);
                            }
                            c->role[k] = 0;
                            break;
                        }
                        q = strchr(q, '&');
                    }
                }
                if (ws_handshake(c, key) < 0) { c->dead = 1; return; }
                c->is_ws = 1;
                c->rx_len = 0;
                WsClientInfo ci;
                memset(&ci, 0, sizeof(ci));      // role is read as a string: never leave it unset
                ci.id = c->id;
                snprintf(ci.peer, sizeof(ci.peer), "%s", c->peer);
                snprintf(ci.role, sizeof(ci.role), "%s", c->role);
                if (g_srv.cfg.on_open) g_srv.cfg.on_open(&ci, g_srv.cfg.user);
                return;
            }
            if (strcmp(method, "GET") != 0) {
                http_send(c, 400, "text/plain", (const unsigned char *)"only GET\n", 9);
                c->dead = 1;
                return;
            }
            http_serve_path(c, path, &g_srv.cfg);
            c->dead = 1;
            return;
        }

        // WebSocket: parse complete frames out of the buffer
        if (c->rx_len < 2) return;
        const unsigned char *b = c->rx;
        uint64_t plen = b[1] & 0x7F;
        int pos = 2;
        if (plen == 126) { if (c->rx_len < 4) return; plen = ((uint64_t)b[2] << 8) | b[3]; pos = 4; }
        else if (plen == 127) { if (c->rx_len < 10) return; plen = 0; for (int i = 0; i < 8; i++) plen = (plen << 8) | b[2+i]; pos = 10; }
        int masked = (b[1] & 0x80) ? 1 : 0;
        uint64_t total = (uint64_t)pos + (masked ? 4 : 0) + plen;
        if (total > (uint64_t)c->rx_len) return;   // need more

        int frame_len = (int)total;
        unsigned char *frame = (unsigned char *)malloc(frame_len);
        if (!frame) { c->dead = 1; return; }
        memcpy(frame, c->rx, frame_len);
        memmove(c->rx, c->rx + frame_len, c->rx_len - frame_len);
        c->rx_len -= frame_len;

        ws_handle_frame(c, frame, frame_len);
        free(frame);
        if (c->dead) return;
    }
}

// ---------------------------------------------------------------------------
// Event loop
// ---------------------------------------------------------------------------
static void *ws_thread(void *arg) {
    server *s = (server *)arg;
    long last_tick = now_ms();

    while (s->running) {
        fd_set rd, wr;
        FD_ZERO(&rd); FD_ZERO(&wr);
        FD_SET(s->listen_fd, &rd);
        int maxfd = s->listen_fd;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client *c = &s->clients[i];
            if (c->fd < 0) continue;
            FD_SET(c->fd, &rd);
            if (c->qbytes > 0) FD_SET(c->fd, &wr);
            if (c->fd > maxfd) maxfd = c->fd;
        }
        struct timeval tv;
        tv.tv_sec = 0; tv.tv_usec = 100000;   // 100 ms
        int r = select(maxfd + 1, &rd, &wr, NULL, &tv);
        if (r < 0 && errno != EINTR) break;

        long t = now_ms();

        // Tick callback (state push)
        if (s->cfg.on_tick && s->cfg.tick_ms > 0 && (t - last_tick) >= s->cfg.tick_ms) {
            last_tick = t;
            s->cfg.on_tick(s->cfg.user);
        }

        // New connection
        if (r > 0 && FD_ISSET(s->listen_fd, &rd)) {
            struct sockaddr_in sa;
            socklen_t sl = sizeof(sa);
            int fd = accept(s->listen_fd, (struct sockaddr *)&sa, &sl);
            if (fd >= 0) {
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++) if (s->clients[i].fd < 0) { slot = i; break; }
                if (slot < 0) {
                    const char *busy = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                    (void)!write(fd, busy, strlen(busy));
                    close(fd);
                } else {
                    client *c = &s->clients[slot];
                    memset(c, 0, sizeof(*c));
                    c->fd = fd;
                    c->id = slot;
                    c->last_ms = t;
                    snprintf(c->peer, sizeof(c->peer), "%s:%d", inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
                    int one = 1;
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    set_nonblock(fd);
                }
            }
        }

        // Client I/O
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client *c = &s->clients[i];
            if (c->fd < 0) continue;

            if (r > 0 && FD_ISSET(c->fd, &rd)) {
                if (rx_reserve(c, c->rx_len + 4096) < 0) { c->dead = 1; }
                else {
                    ssize_t n = recv(c->fd, c->rx + c->rx_len, 4096, 0);
                    if (n > 0) {
                        c->rx_len += (int)n;
                        c->last_ms = t;
                        client_process(c);
                    } else if (n == 0) {
                        client_drop(c, 1);
                        continue;
                    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                        client_drop(c, 1);
                        continue;
                    }
                }
            }

            if (c->qbytes > 0) {
                pthread_mutex_lock(&s->mtx);
                client_queue_flush(c);
                pthread_mutex_unlock(&s->mtx);
            }

            if (c->dead) { client_drop(c, 1); continue; }
            // Only time out clients that have not completed a request yet
            // (an idle WebSocket client is perfectly normal).
            if (!c->is_ws && (t - c->last_ms) > CLIENT_TIMEOUT_MS) { client_drop(c, 1); continue; }
        }
    }
    return NULL;
}

void *ws_start(const WsConfig *cfg) {
    if (g_srv.started) return g_handle;
    memset(&g_srv, 0, sizeof(g_srv));
    g_srv.cfg = *cfg;
    g_srv.port = cfg->port;
    g_srv.listen_fd = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) g_srv.clients[i].fd = -1;
    pthread_mutex_init(&g_srv.mtx, NULL);
    signal(SIGPIPE, SIG_IGN);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)cfg->port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "m17web: cannot bind port %d: %s\n", cfg->port, strerror(errno));
        close(fd);
        return NULL;
    }
    if (listen(fd, 8) < 0) {
        fprintf(stderr, "m17web: listen failed: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }
    set_nonblock(fd);
    g_srv.listen_fd = fd;
    g_srv.running = 1;
    g_srv.started = 1;
    if (pthread_create(&g_srv.tid, NULL, ws_thread, &g_srv) != 0) {
        g_srv.running = 0;
        close(fd);
        g_srv.started = 0;
        return NULL;
    }
    return g_handle;
}

void ws_stop(void *handle) {
    (void)handle;
    if (!g_srv.started) return;
    g_srv.running = 0;
    pthread_join(g_srv.tid, NULL);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_srv.clients[i].fd >= 0) client_drop(&g_srv.clients[i], 0);
    if (g_srv.listen_fd >= 0) close(g_srv.listen_fd);
    g_srv.listen_fd = -1;
    g_srv.started = 0;
}
