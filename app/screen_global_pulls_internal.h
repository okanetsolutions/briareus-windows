#ifndef BRIAREUS_SCREEN_GLOBAL_PULLS_INTERNAL_H
#define BRIAREUS_SCREEN_GLOBAL_PULLS_INTERNAL_H
#include "screen_pulls_internal.h"
#include "global_pulls.h"
enum { GLOBAL_POLL = 1, GLOBAL_REFRESH = 2000, GLOBAL_MORE, GLOBAL_OPEN, GLOBAL_CLEAR, GLOBAL_STACK, GLOBAL_CHECKS, GLOBAL_FILTER, GLOBAL_MORE_ROWS, GLOBAL_FILTER_BASE = 2100 };
typedef struct GlobalPullsScreen {
    Screen base;
    struct GlobalPullsScreen *next_live;
    ApiClient *client; Json *device;
    GlobalPulls state;
    BoardFilter filter; bool hide_stacked;
    Request *req;
    Poller poller;
    bool shown, listed, loading, fresh, queued_fresh, reading_projects;
    size_t limit, cursor, visible_rows;
    time_t next_poll, projects_retry;
    int saved_scroll, selected_number; bool restore_scroll; char *selected_repo;
    char *error;
    GlobalPullRow *rows; size_t row_count;
    intptr_t action_first, action_next;
} GlobalPullsScreen;
bool global_pulls_current(GlobalPullsScreen *s);
void global_pulls_changed(GlobalPullsScreen *s);
void global_pulls_register(GlobalPullsScreen *s);
void global_pulls_unregister(GlobalPullsScreen *s);
void global_pulls_track(GlobalPullsScreen *s);
time_t global_pulls_projects_failed(const ApiError *error);
time_t global_pulls_projects_deadline(void);
void global_pulls_projects_succeeded(void);
void global_pulls_load(GlobalPullsScreen *s, bool fresh);
void global_pulls_next(GlobalPullsScreen *s);
void global_pulls_layout(Screen *base, Doc *doc);
void global_pulls_header(Screen *base, HeaderInfo *info);
void global_pulls_pick(GlobalPullsScreen *s, FilterKind kind, POINT pt);
void global_pulls_filters_save(GlobalPullsScreen *s);
Screen *global_pulls_detail(GlobalPullsScreen *s, intptr_t arg);
#endif
