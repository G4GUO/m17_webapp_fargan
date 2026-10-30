// ui.h
//
// Presentation layer of m17web.
//
// The protocol and DSP code talks to the outside world through the
// (historically named) gui_* entry points below.  Two back ends implement
// this API:
//
//   webui.cpp       browser UI: HTTP + WebSocket server (default)
//   ui_console.cpp  original ncurses terminal UI (optional, -DCONSOLE=1)
//
// Nothing in the M17/DSP code needs to know which one is in use.
//
#ifndef UI_H
#define UI_H

/* Push a line to the operator log the interface shows (the browser console or
 * the ncurses log pane).  printf() only reaches the journal, which is no use to
 * someone watching the station. */
void gui_log_event(const char *kind, const char *fmt, ...);

#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Which back end was compiled in
// ---------------------------------------------------------------------------
typedef enum { UI_NONE = 0, UI_BACKEND_WEB, UI_BACKEND_CONSOLE } UiBackend;

UiBackend ui_backend(void);

// Startup configuration, set by main() from the command line before gui_open()
void ui_set_web_port(int port);
void ui_set_www_dir(const char *dir);   // NULL/"" = use embedded assets only
int  ui_get_web_port(void);
const char *ui_get_www_dir(void);

// ---------------------------------------------------------------------------
// Back end entry points (both web and console implement all of these)
// ---------------------------------------------------------------------------
void gui_open(void);                     // bring the UI up
void gui_close(void);                    // shut the UI down
void gui_update(void);                   // re-render / push state to clients
void gui_update_clear(void);             // clear the screen / log pane
void gui_los(void);                      // loss of signal
void gui_aos(void);                      // acquisition of signal
void gui_cmd_prompt(void);               // console: reprint prompt  (web: no-op)
void gui_cmd_resp(const char *resp);     // command response line
void gui_help(void);                     // print / push the command help
void gui_bar(double v);                  // legacy combined meter
void gui_signal(double v);               // received signal level 0..1
void gui_power(double v);                // transmit power level 0..1
void gui_char(char c);                   // console: echo typed char (web: no-op)
void gui_save_src_address(uint48_t add);
void gui_save_dest_address(uint48_t add);
void gui_save_mode(const char *mode);
void gui_tx(void);
void gui_rx(void);
void gui_dp(void);

// Own thread while the program runs; returns once the user quits (web: a
// browser 'q' command or SIGINT, console: 'q' typed at the prompt).
void *ui_command_loop(void *arg);

// Help text shared by both back ends
const char *const *ui_help_lines(void);

#endif // UI_H
