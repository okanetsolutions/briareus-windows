#ifndef BRIAREUS_SCREEN_BROWSER_INTERNAL_H
#define BRIAREUS_SCREEN_BROWSER_INTERNAL_H
#include "resource.h"
#include "screens.h"
#include "str.h"
#include <commctrl.h>
#include <math.h>
#include <objidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wincodec.h>
#define WM_BROWSER_FEED (WM_APP + 61)
#define MAX_SIDE 8192
#define WM_BROWSER_MOVE (WM_APP + 62)

// A session's shared browser, as the dashboard's browser panel: the headless Chromium on the server that the session's
// agent drives through Playwright, shown here as the frames its event stream sends and driven with this window's mouse
// and keyboard (`POST …/browser/input`), on the same tabs at the same time. As Claude's browser pane, it docks as a column
// beside the conversation under a compact bar of its own (the tabs and ＋, pop out, ⋯, expand and close, then back, forward,
// reload and the address), and pops out into a window of its own that can dock again.

enum { ACT_START = 1000, ACT_STOP, ACT_BACK, ACT_FORWARD, ACT_RELOAD, ACT_NEW_TAB, ACT_CLOSE_TAB, ACT_TAB, ACT_MORE, ACT_EXPAND,
       ACT_DETACH, ACT_DOCK, ACT_CLOSE_PANE };

enum { TIMER_POLL = 1, TIMER_RECONNECT = 2 };

enum { ID_ADDRESS = 401 };

// MARK: - The feed

/// The event stream, read on a thread of its own and shared with the screen: the latest tabs and picture wait here until
/// the view window takes them, and older ones are dropped. The screen and the thread each hold a reference.
typedef struct {
    volatile LONG refs;
    CRITICAL_SECTION lock;
    StoreStream *stream;
    HWND notify;                                // told when something arrived; NULL once the screen let go
    bool posted;                                // a WM_BROWSER_FEED is on its way
    Json *tabs;                                 // the latest `tabs` event
    unsigned char *pixels; int pw, ph, fw, fh;  // the latest picture: BGRA rows, its pixel size and the page's CSS size
    bool heard, closed, ended;                  // an event came; `closed` came; the stream returned
    ApiError error;
} Feed;

// MARK: - The screen

typedef struct {
    Screen base;
    char *session_id, *title;
    BrowserState state; bool state_read, starting, stopping, closed_session;
    char *error;                         // the last failure of an action, shown over the picture
    char *state_error;                   // why the state could not be read; gone once it or the stream comes through
    Request *req_state, *req_toggle, *req_input;
    Poller poller;                       // reads the state while the browser is down: the agent's next turn starts it
    Feed *feed; int failures; bool shown;
    HWND view, address; RECT view_rc, address_rc;   // content coordinates from the last layout
    unsigned char *pixels; int pw, ph, fw, fh;      // the picture on show
    BrowserRect drawn;                              // where it was last drawn in the view
    BrowserInputs inputs;
    // The pointer: a press becomes a click when it is let go where it started, else a drag of down, moves and up.
    bool pressing, dragging; int press_x, press_y, clicks; const char *button;
    wchar_t high_surrogate;
    bool detached;                       // in a window of its own rather than docked beside the conversation
    char *window_title;                  // that window's title as last set
} BrowserScreen;

enum { MORE_RELOAD = 1, MORE_CLOSE_TAB, MORE_START, MORE_STOP, MORE_DETACH, MORE_DOCK, MORE_TAB = 100 };

Feed *screen_browser_feed_start(const char *session_id, HWND notify);
void screen_browser_feed_stop(Feed *f);
void screen_browser_changed(BrowserScreen *s);
bool screen_browser_can_drive(void);
void screen_browser_show_frame(BrowserScreen *s, unsigned char *pixels, int pw, int ph, int fw, int fh);
void screen_browser_stop_feed(BrowserScreen *s);
void screen_browser_set_address(BrowserScreen *s);
void screen_browser_went_down(BrowserScreen *s);
void screen_browser_read_state(BrowserScreen *s);
void screen_browser_send_input(BrowserScreen *s, Json *input);
Json *screen_browser_simple_input(const char *type);
void screen_browser_add_modifiers(Json *input, bool with_shift);
Json *screen_browser_pointer_input(BrowserScreen *s, const char *type, int x, int y);
void screen_browser_paste(BrowserScreen *s);
LRESULT CALLBACK screen_browser_address_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);
void screen_browser_ensure_controls(BrowserScreen *s);
const char *screen_browser_tab_label(const BrowserTab *tab);
bool screen_browser_conversation_shown(const char *session_id);
bool screen_browser_can_dock(BrowserScreen *s);
void screen_browser_browser_layout(Screen *base, Doc *doc);
void screen_browser_browser_place(Screen *base, const RECT *content, int scroll_y);
bool browser_offered(void);
Screen *screen_browser_browser_new(const char *session_id_, const char *title, bool on, bool running, bool detached);
Screen *browser_screen_new(const Session *session);
void screen_browser_post_move(BrowserScreen *s, int op);
void browser_open(const Session *session);

#endif
