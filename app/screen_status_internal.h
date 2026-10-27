#ifndef BRIAREUS_SCREEN_STATUS_INTERNAL_H
#define BRIAREUS_SCREEN_STATUS_INTERNAL_H
#include "screens.h"
#include "session_cache.h"
#include "session_overview.h"
#include "str.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

enum { STATUS_POLL = 1, STATUS_OPEN = 1400, STATUS_OPTION, STATUS_SEND, STATUS_RESOLVE, STATUS_SERVER };
typedef struct {
    Session session;
    Json *identity, *answered_question;
    Json *unsaved_events; // retained until the entire append succeeds
    Transcript transcript;
    bool present, confirmed, answered, laid;
    char *draft, *error, *read_error;
    HWND edit;
    intptr_t reply_action; // rendered Send action, invalidated with the document
    RECT rect;
} StatusCard;
typedef struct { size_t card; int option; } StatusAction;
typedef struct {
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
} StatusScreen;
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
#endif
