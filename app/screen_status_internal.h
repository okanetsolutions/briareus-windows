#ifndef BRIAREUS_SCREEN_STATUS_INTERNAL_H
#define BRIAREUS_SCREEN_STATUS_INTERNAL_H
#include "screens.h"
#include "session_cache.h"
#include "pull_attention.h"
#include "review_approvals_card.h"
#include "review_outcome_card.h"
#include "session_overview.h"
#include "str.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum { STATUS_POLL = 1, STATUS_ATTENTION_POLL, STATUS_OPEN = 1400, STATUS_OPTION, STATUS_SEND, STATUS_RESOLVE, STATUS_SERVER, STATUS_CI, STATUS_PULL, STATUS_SECURITY };
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
// A pull request waiting on a person's approval (a `review-approval` attention item), approved in place: its review
// outcome from its own `code_reviews` read and the approvals card, whose write it owns. Kept while that write is under
// way, even once attention no longer lists it.
typedef struct {
    StatusScreen *screen;   // borrowed
    char *repo, *summary; int number;
    Json *outcome_state; ReviewOutcome *outcome;
    char *error; bool read, stale;
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
    Request *projects_read, *sessions_read, *question_read, *check_read, *write, *attention_read;
    // Pull requests waiting on a person (`attention`); `pulls_generation` changes whenever the list is replaced, so a
    // click laid out before names the list it was laid out for.
    PullAttention *pulls; size_t pull_count;
    char *attention_error; unsigned pulls_generation; bool attention_loaded;
    double attention_at, attention_retry_until; // monotonic milliseconds; cancellation clears the send time
    ULONGLONG (*clock)(void); // NULL uses GetTickCount64; tests advance deadlines without sleeping.
    StatusApproval **approvals; size_t approval_count;
    Json *send_question, *send_identity;
    char *send_text, *error;
    Poller poller, attention_poller; // Independent completions must not overwrite each other's Retry-After.
    bool loaded, shown;
};
bool status_current(StatusScreen *s);
void status_rebuild(StatusScreen *s);
void status_load(StatusScreen *s);
void status_open_pull(StatusScreen *s, intptr_t arg);
// Reads attention again now, as after an approval landed; `force` ignores the 30-second spacing.
void status_attention_load(StatusScreen *s, bool force);
// The approvals asked for in place (screen_status_approvals.c): kept in step with the attention list, laid out under
// their item, and stopped with the screen.
void status_approvals_merge(StatusScreen *s);
StatusApproval *status_approval_for(StatusScreen *s, const PullAttention *pull);
void status_approval_layout(StatusScreen *s, Doc *doc, int x, int w, StatusApproval *a);
void status_approvals_place(StatusScreen *s, const RECT *content, int scroll_y);
bool status_approvals_action(StatusScreen *s, int action, intptr_t arg);
void status_approvals_cancel(StatusScreen *s);
void status_approvals_free(StatusScreen *s);
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
#endif
