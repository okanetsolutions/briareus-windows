#ifndef BRIAREUS_SCREEN_STATUS_INTERNAL_H
#define BRIAREUS_SCREEN_STATUS_INTERNAL_H
#include "attention.h"
#include "review_approvals_card.h"
#include "review_outcome_card.h"
#include "screens.h"
#include "session_cache.h"
#include "session_overview.h"
#include "str.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

enum { STATUS_POLL = 1, STATUS_OPEN = 1400, STATUS_OPTION, STATUS_SEND, STATUS_RESOLVE, STATUS_SERVER, STATUS_CI, STATUS_APPROVAL_PULL };
// Each pull request waiting on an approval takes APPROVAL_ACTIONS from here, in the order they are listed.
enum { STATUS_APPROVALS_BASE = 1500 };
typedef struct {
    Session session;
    Json *identity, *answered_question;
    Json *unsaved_events; // retained until the entire append succeeds
    Transcript transcript;
    bool present, confirmed, answered, laid;
    char *draft, *error, *read_error;
    HWND label, edit;
    intptr_t reply_action; // rendered Send action, invalidated with the document
    RECT label_rect, rect;
} StatusCard;
typedef struct { size_t card; int option; } StatusAction;
typedef struct StatusScreen StatusScreen;
// A pull request waiting on a person's approval (an `attention` item), read out in place: its review outcome from its own
// `code_reviews` read and the approvals card, whose write it owns. Kept while its write is under way, even once the
// inbox no longer lists it (`present` false).
typedef struct {
    StatusScreen *screen;   // borrowed
    AttentionPull pull;
    Json *outcome_state; ReviewOutcome *outcome;
    char *error; bool read, present, stale;
    Request *req;
    ApprovalForm form;
} StatusApproval;
struct StatusScreen {
    Screen base;
    ApiClient *client; // retained connection identity, never reused for another server/token
    Project *projects; size_t project_count;
    Session *sessions; size_t count, missing;
    SessionOverview overview;
    StatusCard **cards; size_t card_count, next_question, reading_card, sending_card;
    StatusAction *actions; size_t action_count;
    intptr_t action_first, action_next;
    Request *projects_read, *sessions_read, *question_read, *check_read, *write;
    Json *send_question, *send_identity;
    char *send_text, *error;
    Poller poller;
    bool loaded, shown;
    StatusApproval **approvals; size_t approval_count; Request *attention_read; char *attention_error;
};
bool status_current(StatusScreen *s);
void status_rebuild(StatusScreen *s);
void status_load(StatusScreen *s);
void status_questions_next(StatusScreen *s);
void status_cancel_questions(StatusScreen *s);
void status_send(StatusScreen *s, size_t card, const char *text);
bool status_uncertain(StatusScreen *s, StatusCard *c);
void status_set_uncertain(StatusScreen *s, StatusCard *c, bool uncertain);
bool status_can_answer(StatusScreen *s, StatusCard *c);
void status_layout(Screen *base, Doc *doc);
void status_place(Screen *base, const RECT *content, int scroll_y);
void status_command(Screen *base, int id, int code, HWND control);
void status_controls_hide(StatusScreen *s);
intptr_t status_action_add(StatusScreen *s, size_t card, int option);
void status_approvals_load(StatusScreen *s, bool fresh);
void status_approvals_cancel(StatusScreen *s);
void status_approvals_free(StatusScreen *s);
void status_approvals_layout(StatusScreen *s, Doc *doc, int x, int w);
void status_approvals_place(StatusScreen *s, const RECT *content, int scroll_y);
void status_approvals_command(StatusScreen *s, HWND control, int code);
bool status_approvals_action(StatusScreen *s, int action, intptr_t arg);
#endif
