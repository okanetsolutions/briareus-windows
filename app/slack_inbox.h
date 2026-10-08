// Inbox screen state and JSON completion seam, shared with headless app regression tests.
#ifndef BRIAREUS_SLACK_INBOX_H
#define BRIAREUS_SLACK_INBOX_H
#include "screens.h"
#include "slack.h"
#include "slack_feed.h"
enum { ACT_WORKSPACE = 1000, ACT_CHANNEL, ACT_THREAD, ACT_BACK, ACT_PEOPLE, ACT_PERSON, ACT_CONV_MORE,
       ACT_PEOPLE_MORE, ACT_OLDER, ACT_REFRESH, ACT_SEND, ACT_RECOVER, ACT_WEB };
enum { TAG_WORKSPACES, TAG_CONVERSATIONS, TAG_PEOPLE, TAG_DETAIL, TAG_HISTORY, TAG_DM, TAG_SEND, TAG_READ, TAG_SNAPSHOT_CHANNEL, TAG_SNAPSHOT_THREAD, TAG_COUNT };
enum { ID_COMPOSER = 340, TIMER_READ = 40, TIMER_EMPTY = 41, TIMER_LIVE = 42 };

typedef struct SlackScreen SlackScreen;
struct SlackScreen {
    Screen base; SlackScreen *next;
    SlackState state; SlackPage page;
    SlackFeed *feed; uint64_t stream_generation, reconnect_at;
    unsigned stream_failures, snapshot_pending;
    bool reconciling, stream_disabled, workspace_reload;
    SlackEvents buffered;
    SlackState snapshot;
    SlackPage snapshot_channel, snapshot_thread;
    Json *snapshot_conversations;
    char *snapshot_cursor, *snapshot_edge, *live_status, *workspace_user;

    ApiClient *account; char *device; unsigned access;
    Json *workspaces, *conversations, *people, *detail;
    char *workspace, *channel, *thread, *conv_cursor, *people_cursor, *error, *search;
    Request *requests[TAG_COUNT];
    HWND composer; RECT send_rc, edit_rc;
    bool shown, filling, directory, loaded, history_loaded, read_failed;
    uint64_t cooldown, display_revision, layout_revision; unsigned empty_pages; bool laid_out;
    struct { RECT rc; char *ts; } *view; size_t view_count;
};
void slack_inbox_focus(SlackScreen *s);
void slack_inbox_event(SlackScreen *s, uint64_t generation, const char *name, const char *data, size_t length);
void slack_inbox_stream_end(SlackScreen *s, uint64_t generation, const ApiError *error);
void slack_inbox_pump(SlackScreen *s);
void slack_inbox_done(void *owner, Request *request);
void slack_inbox_recover_confirmed(SlackScreen *screen, const Json *destination, uint64_t generation);
void slack_inbox_find_confirmed(SlackScreen *screen, const char *workspace, uint64_t generation, const char *query);
#endif
