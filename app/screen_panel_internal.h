#ifndef BRIAREUS_SCREEN_PANEL_INTERNAL_H
#define BRIAREUS_SCREEN_PANEL_INTERNAL_H
#include "screens.h"
#include "briareus_tokens.h"
#include "canvas.h"
#include "dialogs.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The dashboard's `#pr-panel`: the column on the right of a conversation with its pull request, commits and reviews,
// and under them the session's context usage.

enum {
    ACT_OPEN_PR = 1000, ACT_VIEW_PR, ACT_COMMITS, ACT_COMMIT_URL,
    ACT_AUTO_COMPACT, ACT_INSTRUCTIONS, ACT_COMPACT, ACT_CLEAR,
};

enum { TIMER_POLL = 1 };

enum { TAG_PULL = 1 };

typedef struct {
    Screen base;
    Session session;
    Project project; int number;   // number 0 until the session has a pull request
    Json *pr;
    char *error;
    Request *req_pull;
    Poller poller;
    bool commits_open;
} PanelScreen;

void screen_panel_pr_layout(PanelScreen *s, Doc *doc, int w);
bool session_panel_wanted(const Session *session);
void screen_panel_usage_layout(PanelScreen *s, Doc *doc, int w, bool after_pr);
Screen *session_panel_screen_new(const Session *session);
void session_panel_update(const Session *session);

#endif
