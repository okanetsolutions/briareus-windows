#ifndef BRIAREUS_SCREEN_WEBHOOK_INTERNAL_H
#define BRIAREUS_SCREEN_WEBHOOK_INTERNAL_H
#include "screens.h"
#include "str.h"
#include "webhook.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A session's ⚡ Webhook, as the dashboard's dialog: whether an outside system may post to wake the session, the caps
// the turns it starts run under, the second webhook that takes the operator's own instructions, and the URLs and keys a
// sender needs, with Rotate key to end the old ones. An admin token's, read and written through /sessions/{id}/webhook.

enum { F_PER_HOUR = WEBHOOK_PER_HOUR, F_MAX_TURNS = WEBHOOK_MAX_TURNS, F_COUNT = WEBHOOK_FIELD_COUNT };

enum { T_ARMED, T_INSTRUCTIONS, T_SSH, T_COUNT };

extern const char *const screen_webhook_TOGGLE_KEYS[T_COUNT];
enum { ACT_SAVE = 1200, ACT_ROTATE, ACT_TOGGLE, ACT_FOCUS, ACT_COPY, ACT_SHOW, ACT_RETRY };

enum { ID_FIELD = 2400 };

enum { TAG_READ = 1, TAG_WRITE = 2 };

// What ACT_COPY and ACT_SHOW are about.
enum { V_URL, V_KEY, V_INSTRUCTIONS_URL, V_INSTRUCTIONS_KEY };

typedef struct {
    Screen base;
    Session session;
    Json *hook;             // the server's last answer, a Webhook
    bool toggles[T_COUNT];
    bool shown_keys[4];
    bool loading, busy, dirty, filling, shown;
    bool modal, retired;    // Defer release while a leave confirmation pumps messages.
    char *error;            // the read failed
    char *message; bool message_ok;
    Request *req_read, *req_write;
    HWND edits[F_COUNT];
    RECT rects[F_COUNT];
    bool laid[F_COUNT], clipped[F_COUNT];
    int focused;
} WebhookForm;

const char *screen_webhook_hook_str(WebhookForm *s, const char *key);
bool screen_webhook_hook_armed(WebhookForm *s);
char *screen_webhook_edit_text(HWND edit);
void screen_webhook_form_fill(WebhookForm *s);
void screen_webhook_form_save(WebhookForm *s);
void screen_webhook_form_layout(Screen *base, Doc *doc);
void screen_webhook_form_header(Screen *base, HeaderInfo *info);
void screen_webhook_form_place(Screen *base, const RECT *content, int scroll_y);
void screen_webhook_form_ensure_controls(WebhookForm *s);
Screen *webhook_screen_new(const Session *session);

#endif
