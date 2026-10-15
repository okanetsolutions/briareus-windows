#ifndef BRIAREUS_PROJECT_RUN_INTERNAL_H
#define BRIAREUS_PROJECT_RUN_INTERNAL_H
#include "dialogs.h"
#include "screens.h"
#include "str.h"
#include "webview.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A project's Run tab on its board, after Issues: the pull request's Run tab, on the project's default branch. Opening it
// serves the branch in a clean workspace with the project's run commands (`serve_branch`, no agent turn) and shows it in
// an embedded browser under an address bar, with the setup's console until the page is up. The header picks the run
// profile and deletes the run with its workspace.

// The actions, from the host's `action_base` up.
enum { A_PROFILE, A_RELOAD, A_BROWSER, A_RETRY, A_DELETE, A_BACK, A_FORWARD };

struct ProjectRun {
    char *repo;
    Screen *host;
    int base; UINT timer;
    // `session` serves the branch at `url` with `profile`; `want` is the profile picked in the header, `asked` the one
    // the request in flight asked for. `branch` is what the server served, once it said.
    char *url, *session, *profile, *want, *asked, *branch, *serve_error, *delete_error;
    bool busy, pending, sessions_read, deleting;   // pending: waits for the sessions, in case a Run already serves the branch
    char **profiles; size_t profile_count; bool profiles_read;   // the project's run profiles, the default first
    // The log of the Run under way, from its session's transcript: `log_session` is the session followed (found among the
    // project's sessions while `serve_branch` prepares it).
    char *log_session; RunLog log;
    WebView *web; RECT web_rc; bool shown; RunAddress address;
    // The Cloudflare Access service token the browser sends to preview hosts, read once before it first opens.
    char *access_id, *access_secret, *access_suffix; bool access_read;
    Request *req_run, *req_profiles, *req_log, *req_sessions, *req_delete, *req_access;
};

bool project_run_offered(void);
void project_run_changed(ProjectRun *p);
void project_run_web_changed(void *ctx);
void project_run_timer_on(ProjectRun *p, bool on);
const char *project_run_target(ProjectRun *p);
const char *project_run_shown_profile(ProjectRun *p);
void project_run_open(ProjectRun *p);
void run_address_place(RunAddress *a, Screen *host, WebView **web, const char *url, const RECT *content, int scroll_y, bool shown);
void run_address_free(RunAddress *a);
void run_browser_bar(Doc *doc, int w, WebView *web, const char *url, const int actions[4], RunAddress *address);
void project_run_layout(ProjectRun *p, Doc *doc, int w);
void project_run_header(ProjectRun *p, HeaderInfo *info);
void project_run_place(ProjectRun *p, const RECT *content, int scroll_y, bool shown);
void project_run_refresh(ProjectRun *p);
bool project_run_timer(ProjectRun *p, UINT id);
bool project_run_action(ProjectRun *p, int action, intptr_t arg, POINT pt);
ProjectRun *project_run_new(const char *repo, Screen *host, int action_base, UINT timer);
void project_run_free(ProjectRun *p);

#endif
