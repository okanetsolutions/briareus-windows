#ifndef BRIAREUS_SCREEN_CONVERSATION_INTERNAL_H
#define BRIAREUS_SCREEN_CONVERSATION_INTERNAL_H
#include "saved_prompts.h"
#include "attach_list.h"
#include "dialogs.h"
#include "image_cache.h"
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
    ACT_ANSWER = 1000, ACT_TOOL_TOGGLE, ACT_DROP_QUEUED, ACT_REFRESH_OUTCOME, ACT_DISMISS_ERROR, ACT_REOPEN,
    ACT_PICTURE_OPEN, ACT_PICTURE_ALLOW, ACT_PICTURE_RETRY,   // a picture under a reply; the arg is its address
    ACT_MENU_ITEM = 1100,   // plus a MENU_ id: the header's buttons
};

enum { TIMER_POLL = 1, TIMER_WORKING = 2, TIMER_VOICE = 3 };

enum { ID_COMPOSER = 301 };

enum { TAG_REFRESH = 1, TAG_MUTATE = 2 };

enum { MENU_CHANGES = 1, MENU_PULL, MENU_RENAME, MENU_STOP, MENU_CLOSE, MENU_REOPEN, MENU_DELETE, MENU_COPY, MENU_BROWSER, MENU_WEBHOOK, MENU_MAIL_WAKE, MENU_RESOURCES };

typedef struct {
    Screen base;
    Session initial, snapshot; bool has_snapshot;
    Transcript transcript;
    bool loaded, restored, unsaved, retimed, removed;
    bool busy, loading, uncertain, pending_full, pending_refresh, renaming, dialog_open;
    char *error, *write_error;
    char *pending_mutation;   // the operation a confirmation is up for
    Request *req_refresh, *req_mutate;
    Poller poller;
    HWND composer; int composer_lines; RECT composer_rc;
    bool composer_focused_once;
    int *expanded; size_t expanded_count;   // tool rows opened
    int tick;
    VoiceNote *voice;
    PromptPicker *prompts;
    AttachList *files;
    char header_title[512], header_subtitle[256];
    // footer hit rects
    RECT mic_rc, send_rc, discard_rc, attach_rc, prompts_rc;
    bool at_bottom;
    int return_scroll;   // where the view was when a picture was opened from above the end; -1 otherwise
} ConversationScreen;

// MARK: - Footer (composer)

enum { CHIP_WORKSPACE, CHIP_PROJECT, CHIP_BRANCH, CHIP_PROVIDER, CHIP_MODEL, CHIP_EFFORT, CHIP_COUNT };

const Session *screen_conversation_session(ConversationScreen *s);
bool screen_conversation_review_removed(ConversationScreen *s, const Request *req);
bool screen_conversation_can_message(ConversationScreen *s);
bool screen_conversation_can_compose(ConversationScreen *s);
void screen_conversation_prompts_init(ConversationScreen *s);
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
/// A reply's picture, as the document's `picture` layout: its preview, which opens it, or what it waits for.
void screen_conversation_layout_picture(Doc *doc, int x, int w, const MdImage *image, void *ctx);
/// The image cache's watcher: lays the conversation out again.
void screen_conversation_pictures_changed(void *ctx);
/// True when the action was a picture's.
bool screen_conversation_picture_action(ConversationScreen *s, int action, intptr_t arg);
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
