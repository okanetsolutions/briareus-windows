#ifndef BRIAREUS_SCREEN_CONVERSATION_INTERNAL_H
#define BRIAREUS_SCREEN_CONVERSATION_INTERNAL_H
#include "attach_list.h"
#include "dialogs.h"
#include "meeting.h"
#include "screens.h"
#include "str.h"
#include "session_working.h"
#include "voice.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One conversation: its transcript, the composer, and the actions on the session.

enum {
    ACT_ANSWER = 1000, ACT_TOOL_TOGGLE, ACT_DROP_QUEUED, ACT_TRIAGE_DECISION, ACT_TRIAGE_COMPLETE, ACT_TRIAGE_NOTE,
    ACT_REFRESH_OUTCOME, ACT_DISMISS_ERROR, ACT_REOPEN, ACT_FINDING_LINK,
    ACT_MENU_ITEM = 1100,   // plus a MENU_ id: the header's buttons
};

enum { TIMER_POLL = 1, TIMER_WORKING = 2, TIMER_VOICE = 3 };

enum { ID_COMPOSER = 301 };

enum { TAG_REFRESH = 1, TAG_MUTATE = 2 };

enum { MENU_CHANGES = 1, MENU_PULL, MENU_LOOP_ON, MENU_LOOP_OFF, MENU_RENAME, MENU_STOP, MENU_CLOSE, MENU_REOPEN, MENU_DELETE, MENU_COPY, MENU_BROWSER, MENU_WEBHOOK, MENU_RESOURCES };

typedef struct {
    Screen base;
    Session initial, snapshot; bool has_snapshot;
    Transcript transcript;
    bool loaded, restored, unsaved, retimed;
    bool busy, loading, uncertain, pending_full, pending_refresh, renaming, dialog_open;
    bool reloading;   // a mutation landed and the session it changed is not read yet
    char *error, *write_error;
    char *pending_mutation;   // the operation a confirmation is up for
    Request *req_refresh, *req_mutate;
    Poller poller;
    HWND composer; int composer_lines; RECT composer_rc;
    const char *cost_text; int cost_width, cost_unit, cost_h;   // the loop cost last measured
    bool composer_focused_once;
    int *expanded; size_t expanded_count;   // tool rows opened
    char **decisions; size_t decision_count;  // triage picks, "key=decision"
    char *triage_note;
    int tick;
    VoiceNote *voice;
    AttachList *files;
    char header_title[512], header_subtitle[256];
    // footer hit rects
    RECT mic_rc, send_rc, discard_rc, attach_rc;
    bool at_bottom;
} ConversationScreen;

// MARK: - Footer (composer)

enum { CHIP_WORKSPACE, CHIP_PROJECT, CHIP_BRANCH, CHIP_PROVIDER, CHIP_MODEL, CHIP_EFFORT, CHIP_LOOP, CHIP_COUNT };

const Session *screen_conversation_session(ConversationScreen *s);
bool screen_conversation_can_message(ConversationScreen *s);
char *screen_conversation_composer_text(ConversationScreen *s);
void screen_conversation_set_composer_text(ConversationScreen *s, const char *text);
bool screen_conversation_composer_empty(ConversationScreen *s);
void screen_conversation_send_message(ConversationScreen *s);
LRESULT CALLBACK screen_conversation_composer_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);
void screen_conversation_refresh(ConversationScreen *s, bool full);
void screen_conversation_mutate(ConversationScreen *s, const char *name, Json *extra);
void screen_conversation_confirm_and_mutate(ConversationScreen *s, const char *action);
void screen_conversation_voice_changed(void *ctx);
bool screen_conversation_is_expanded(ConversationScreen *s, int seq);
void screen_conversation_toggle_expanded(ConversationScreen *s, int seq);
void screen_conversation_set_decision(ConversationScreen *s, const char *key, const char *decision);
int screen_conversation_column_x(int width);
int screen_conversation_column_w(int width);
void screen_conversation_ev_log(Doc *doc, int x, int w, const char *text, COLORREF color);
void screen_conversation_doc_summary(Doc *doc, int x, int w, const char *text, bool open, int action, intptr_t arg);
void screen_conversation_paint_left_border(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
const char *screen_conversation_tool_title(const Event *e);
void screen_conversation_layout_step(Doc *doc, int x, int w, const Event *e);
bool screen_conversation_is_tool(const Event *e);
bool screen_conversation_is_prep_line(const Event *e);
bool screen_conversation_prep_closed(const Event *e);
void screen_conversation_paint_queued_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
void screen_conversation_paint_working(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
void screen_conversation_layout_event(ConversationScreen *s, Doc *doc, int x, int w, const Event *e);
void screen_conversation_layout_triage(ConversationScreen *s, Doc *doc, int x, int w, const Json *triage);
void screen_conversation_complete_triage(ConversationScreen *s, size_t fixes);
void screen_conversation_conversation_layout(Screen *base, Doc *doc);
void screen_conversation_conversation_header(Screen *base, HeaderInfo *info);
void screen_conversation_menu_choice(ConversationScreen *s, int chosen);
void screen_conversation_conversation_action(Screen *base, int action, intptr_t arg, POINT pt);
int screen_conversation_conversation_footer_height(Screen *base, int width);
void screen_conversation_conversation_footer_layout(Screen *base, const RECT *rc);
void screen_conversation_conversation_footer_paint(Screen *base, Canvas *cv, const RECT *rc);
void screen_conversation_conversation_footer_click(Screen *base, POINT pt);
void screen_conversation_conversation_command(Screen *base, int id, int code, HWND control);
void conversation_session_op(const char *session_id_, const char *operation, Json *extra);
Screen *conversation_screen_new(const Session *initial);

#endif
