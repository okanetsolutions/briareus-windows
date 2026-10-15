#ifndef BRIAREUS_SCREEN_FINDINGS_INTERNAL_H
#define BRIAREUS_SCREEN_FINDINGS_INTERNAL_H
#include "dialogs.h"
#include "screens.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The findings waiting for a decision across every project the device may see, as the dashboard's Findings screen
// queues them: each review round held on a conversation, grouped by the pull request it was left on. A round on the
// user's own pull request takes a verdict on every finding and is completed from here, which starts the fix session;
// a review of somebody else's is read, replied to on its findings' threads, and taken off the queue.

enum {
    ACT_OPEN_SESSION = 1000, ACT_OPEN_PR, ACT_OPEN_FINDING, ACT_VERDICT, ACT_COMMENT, ACT_REPLY, ACT_DELETE, ACT_FIX_ALL,
    ACT_CLEAR_ALL, ACT_NOTE, ACT_SAVE, ACT_COMPLETE, ACT_DISMISS_OUTCOME, ACT_OUTCOME_SESSION, ACT_REPLIED_URL, ACT_SAVED_URL,
};

enum { TIMER_POLL = 1 };

enum { TAG_PROJECTS = 1, TAG_SESSIONS, TAG_WRITE };

enum { FINDING_STRIDE = 1024 };

// a finding's argument is round * stride + finding; a verdict's is that times 4 plus the segment
enum { WRITE_NONE, WRITE_COMPLETE, WRITE_SAVE, WRITE_REPLY, WRITE_DELETE };

typedef struct { char *session_id; int round; char *key; char *decision; char *reason; } Draft;

// decision NULL while unmarked
typedef struct { char *session_id; int round; char *note; } Note;

typedef struct { char *session_id; char *key; char *url; char *error; } Replied;

// what a reply on a finding came back with
typedef struct { char *session_id; char *error; char *saved; char *saved_url; bool saved_failed; char *info; bool info_danger; } Card;

typedef struct { char *session_id; char *title; char *text; bool danger; } Outcome;

typedef struct {
    Screen base;
    Project *projects; size_t project_count;
    Session *all; size_t all_count;             // every project's conversations, which the rounds index
    Session *incoming; size_t incoming_count;   // what the read under way has gathered so far
    HeldRound *rounds; size_t round_count;
    bool loaded, doc_stale, cycle_failed, projects_failed;
    char *error;
    Request *req_projects, *req_sessions, *req_write;
    double retry_after;
    Poller poller;
    Draft *drafts; size_t draft_count;
    Note *notes; size_t note_count;
    char **seeded; size_t seeded_count;         // "session\nround" of the rounds whose saved drafts were taken once
    Replied *replied; size_t replied_count;
    Card *cards; size_t card_count;
    Outcome *outcomes; size_t outcome_count;
    int write_kind; char *write_session; char *write_key; char *write_title; int write_round;
    char *unsent, *unsent_title; int unsent_round;   // a completion that failed, whose card may be gone once the list is read again
} FindingsScreen;

int screen_findings_round_number(const Json *held);
const Session *screen_findings_round_session(FindingsScreen *s, size_t round);
int screen_findings_find_round(FindingsScreen *s, const char *sid, int round);
Draft *screen_findings_draft_find(FindingsScreen *s, const char *sid, int round, const char *key);
Draft *screen_findings_draft_get(FindingsScreen *s, const char *sid, int round, const char *key);
void screen_findings_draft_clear(Draft *d);
const char *screen_findings_decision_of(FindingsScreen *s, const char *sid, int round, const Json *finding);
const char *screen_findings_reason_of(FindingsScreen *s, const char *sid, int round, const Json *finding);
void screen_findings_note_set(FindingsScreen *s, const char *sid, int round, const char *text);
const char *screen_findings_note_of(FindingsScreen *s, const char *sid, int round);
Replied *screen_findings_replied_find(FindingsScreen *s, const char *sid, const char *key);
Replied *screen_findings_replied_get(FindingsScreen *s, const char *sid, const char *key);
void screen_findings_replied_clear(Replied *r);
void screen_findings_replied_drop(FindingsScreen *s, const char *sid, const char *key);
Card *screen_findings_card_find(FindingsScreen *s, const char *sid);
Card *screen_findings_card_get(FindingsScreen *s, const char *sid);
void screen_findings_card_clear(Card *c);
void screen_findings_card_forget_saved(FindingsScreen *s, const char *sid);
void screen_findings_outcome_clear(Outcome *o);
void screen_findings_outcome_set(FindingsScreen *s, const char *sid, const char *title, const char *text, bool danger);
void screen_findings_outcome_drop(FindingsScreen *s, size_t i);
void screen_findings_drop_round_drafts(FindingsScreen *s, const char *sid, int round);
void screen_findings_rebuild(FindingsScreen *s);
void screen_findings_cycle_now(FindingsScreen *s);
size_t findings_waiting(const Project *projects, size_t count);
bool screen_findings_writing(FindingsScreen *s, int kind, const char *sid, const char *key);
void screen_findings_complete_round(FindingsScreen *s, size_t r);
void screen_findings_save_round(FindingsScreen *s, size_t r);
void screen_findings_reply_on(FindingsScreen *s, size_t r, size_t finding);
void screen_findings_delete_finding(FindingsScreen *s, size_t r, size_t finding);
void screen_findings_findings_layout(Screen *base, Doc *doc);
void screen_findings_findings_header(Screen *base, HeaderInfo *info);
Screen *findings_screen_new(void);

#endif
