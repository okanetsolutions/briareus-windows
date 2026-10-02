// A session's shared browser (`/sessions/{id}/browser`): the Chromium its agent drives and the app watches and drives too.
// The pieces with no window in them: the server-sent event stream's parser, the frame's base64, the tabs, where a frame is
// drawn and how a pointer maps back to the page, the keys the server takes, and the queue input waits in.
#ifndef BRIAREUS_BROWSER_H
#define BRIAREUS_BROWSER_H
#include "json.h"
#include "str.h"
#include <stdbool.h>
#include <stddef.h>

// MARK: - Server-sent events

/// One event as it completes: its `event:` name ("message" when it had none) and its `data:` lines joined by newlines.
typedef void (*SseEmit)(void *ctx, const char *event, const char *data, size_t len);
/// Reads an event stream fed in pieces of any size, as text/event-stream lays it out: lines ending in CR, LF or CRLF,
/// `field: value`, comments starting with a colon, and a blank line ending each event.
typedef struct { Str line, data; char *event; bool after_cr, has_data, overflow; } SseParser;
/// The longest line or event kept; past it, the event is dropped (a frame is a few hundred kilobytes).
#define SSE_MAX_EVENT (32u * 1024 * 1024)
void sse_init(SseParser *p);
void sse_free(SseParser *p);
void sse_feed(SseParser *p, const char *bytes, size_t len, SseEmit emit, void *ctx);

/// Standard base64, padding optional; NULL for anything else. `*len` is the decoded size.
unsigned char *base64_decode(const char *text, size_t text_len, size_t *len);

// MARK: - State

typedef struct { char *id, *url, *title; } BrowserTab;
typedef struct {
    bool on, running;
    BrowserTab *tabs; size_t count;
    char *active;   // the tab in view, or NULL
} BrowserState;
void browser_state_free(BrowserState *s);
/// Takes `tabs` and `active` from a `tabs` event or a Browser record, and `on` and `running` when the record has them.
void browser_state_read(BrowserState *s, const Json *record);
/// The tab in view, or NULL.
const BrowserTab *browser_active_tab(const BrowserState *s);
/// A session record's `browser`: false while it is off (null or absent); `*running` says whether it is up.
bool browser_session_on(const Json *session, bool *running);

// MARK: - Drawing and pointing

/// Where a `frame_w` × `frame_h` picture goes inside a `view_w` × `view_h` area: as large as fits without stretching,
/// never larger than the area, centred. An empty rectangle when either has no size.
typedef struct { int left, top, right, bottom; } BrowserRect;
BrowserRect browser_fit(int view_w, int view_h, int frame_w, int frame_h);
/// The page point under a view point, in the frame's CSS pixels: false when the point is outside the drawn picture.
bool browser_page_point(const BrowserRect *drawn, int frame_w, int frame_h, int x, int y, double *page_x, double *page_y);

/// The name the server takes in a `key` input for a virtual key, or NULL for a key it has none for. Letters and digits
/// come back as one lowercase or digit character, for shortcuts with ctrl or alt.
const char *browser_key_name(unsigned vk);

// MARK: - Input

/// Input waiting to go to `POST …/browser/input`, oldest first, sent one at a time so it arrives in order. Typing is
/// merged into one `type`, a pointer move replaces the move before it and scrolls add up, so a slow connection catches up.
typedef struct { Json **items; size_t count, cap; } BrowserInputs;
void browser_inputs_free(BrowserInputs *q);
/// Takes ownership of `input`.
void browser_inputs_push(BrowserInputs *q, Json *input);
/// The oldest input, now the caller's, or NULL.
Json *browser_inputs_pop(BrowserInputs *q);
/// A URL as typed into the address field: https:// added when it has no scheme; NULL when it is empty or not http(s).
char *browser_address(const char *typed);

#endif
