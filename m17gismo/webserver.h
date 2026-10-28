// webserver.h
//
// Small self-contained HTTP + WebSocket server (no external dependencies).
// One instance per process; started from a background thread.
//
//   * serves static files from a directory (and/or embedded assets)
//   * /ws is the WebSocket end point
//   * callbacks deliver client connect/disconnect and messages
//   * a periodic tick callback lets the caller push state updates
//
#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int   id;                     // client id (stable for the connection)
    char  peer[64];               // ip:port of the client
    char  role[16];               // "phone" for /ws?client=phone, else ""
} WsClientInfo;

typedef void (*ws_open_cb)(const WsClientInfo *ci, void *user);
typedef void (*ws_close_cb)(int client_id, void *user);
// is_binary: 1 = binary frame, 0 = text frame
typedef void (*ws_msg_cb)(int client_id, int is_binary, const uint8_t *data, int len, void *user);
typedef void (*ws_tick_cb)(void *user);

// Static asset lookup used when a file is not found on disk (embedded www)
typedef struct {
    const char *path;   // e.g. "/app.js"
    const char *mime;
    const unsigned char *data;
    unsigned int len;
} WsAsset;

typedef struct {
    int          port;
    const char  *www_dir;       // NULL = do not read from disk
    const WsAsset *assets;      // NULL terminated table, may be NULL
    ws_open_cb   on_open;
    ws_close_cb  on_close;
    ws_msg_cb    on_msg;
    ws_tick_cb   on_tick;
    int          tick_ms;       // 0 = no tick
    void        *user;
} WsConfig;

void *ws_start(const WsConfig *cfg);   // returns opaque handle, NULL on failure
void  ws_stop(void *handle);

// Send to a single client (returns 0 on success, -1 if the client is gone)
int ws_send_text(void *handle, int client_id, const char *text);
int ws_send_bin(void *handle, int client_id, const uint8_t *data, int len);
// Broadcast helpers (drop data for slow clients rather than blocking)
int ws_broadcast_text(void *handle, const char *text);
int ws_broadcast_bin(void *handle, const uint8_t *data, int len);
// Broadcast only to clients that identified themselves with ?client=<role>
int ws_broadcast_bin_role(void *handle, const char *role, const uint8_t *data, int len);
// The role a client gave, "" if none
const char *ws_client_role(void *handle, int client_id);
int ws_count_role(void *handle, const char *role);
int ws_client_count(void *handle);
// List currently connected clients, returns the number written
int ws_list_clients(void *handle, WsClientInfo *out, int max);

#ifdef __cplusplus
}
#endif

#endif // WEBSERVER_H
