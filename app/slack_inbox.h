// Inbox screen state and JSON completion seam, shared with headless app regression tests.
#ifndef BRIAREUS_SLACK_INBOX_H
#define BRIAREUS_SLACK_INBOX_H
#include "screens.h"
#include "slack.h"
enum { ACT_WORKSPACE = 1000, ACT_CHANNEL, ACT_THREAD, ACT_BACK, ACT_PEOPLE, ACT_PERSON, ACT_CONV_MORE,
       ACT_PEOPLE_MORE, ACT_OLDER, ACT_REFRESH, ACT_SEND, ACT_RECOVER, ACT_WEB };
enum { TAG_WORKSPACES, TAG_CONVERSATIONS, TAG_PEOPLE, TAG_DETAIL, TAG_HISTORY, TAG_DM, TAG_SEND, TAG_READ, TAG_COUNT };
enum { ID_COMPOSER = 340, TIMER_READ = 40, TIMER_EMPTY = 41 };

typedef struct SlackScreen SlackScreen;
struct SlackScreen {
    Screen base; SlackScreen *next;
    SlackState state; SlackPage page;
    ApiClient *account; char *device; unsigned access;
    Json *workspaces, *conversations, *people, *detail;
    char *workspace, *channel, *thread, *conv_cursor, *people_cursor, *error, *search;
    Request *requests[TAG_COUNT];
    HWND composer; RECT send_rc, edit_rc;
    bool shown, filling, directory, loaded, history_loaded, read_failed;
    uint64_t cooldown; unsigned empty_pages;
    struct { RECT rc; char *ts; } *view; size_t view_count;
};
void slack_inbox_done(void *owner, Request *request);
#endif
