#ifndef BRIAREUS_SCREEN_PULLS_INTERNAL_H
#define BRIAREUS_SCREEN_PULLS_INTERNAL_H
#include "credentials.h"
#include "dialogs.h"
#include "meeting.h"
#include "markdown.h"
#include "pull_conversation.h"
#include "screens.h"
#include "str.h"
#include "webview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// MARK: - Board

enum { ACT_FILTER = 1000, ACT_TAB, ACT_CLEAR, ACT_OPEN_PULL, ACT_OPEN_ISSUE, ACT_PULL_ACTION, ACT_REFRESH, ACT_FILTER_AUTHOR, ACT_FILTER_REVIEWER, ACT_FILTER_ASSIGNEE, ACT_FILTER_LABEL, ACT_RUNS, ACT_MERGE_PULL, ACT_MEET, ACT_REVIEW_LOGIN, ACT_FILTER_STACK, ACT_FILTER_CHECKS };

enum { ACT_SSH_BASE = 1100 };

// the SSH sessions tab's own actions, PROJECT_SSH_ACTIONS of them
enum { ACT_SFTP_BASE = 1120 };

// the SFTP sessions tab's, PROJECT_SFTP_ACTIONS of them
enum { ACT_RUN_BASE = 1140 };

// the Run tab's, PROJECT_RUN_ACTIONS of them
enum { ACT_DB_BASE = 1160 };

// the Database tab's, PROJECT_DB_ACTIONS of them
enum { ACT_FORGE_BASE = 1180 };

// the Forge tab's, PROJECT_FORGE_ACTIONS of them
enum { ACT_BOARD_BASE = 1200 };

// the Board tab's, BOARD_TAB_ACTIONS of them
enum { ACT_REPO_FILES_BASE = 1220 };

// the Files tab's, PROJECT_FILES_ACTIONS of them
enum { TAB_PULLS, TAB_ISSUES, TAB_SSH, TAB_SFTP, TAB_RUN, TAB_DB, TAB_FORGE, TAB_MEETING, TAB_BOARD, TAB_REVIEW, TAB_FILES };

enum { TIMER_POLL = 1, TIMER_BOARD_RUN_LOG = 3, TIMER_FILES_GOTO = 4, TIMER_LIST_RETRY = 5 };

enum { ACTION_STRIDE = 64 };

typedef struct {
    Screen base;
    Project project;
    Json *board;
    PullSummary *pulls; size_t pull_count;
    IssueSummary *issues; size_t issue_count;
    int tab;   // TAB_PULLS, TAB_ISSUES, TAB_BOARD, TAB_REVIEW, TAB_FILES, TAB_RUN, TAB_SSH, TAB_SFTP, TAB_DB, TAB_FORGE or TAB_MEETING
    ProjectSsh *ssh;   // the SSH sessions tab
    ProjectSftp *sftp; // the SFTP sessions tab
    ProjectRun *run;   // the Run tab, on the default branch
    ProjectDb *db;     // the Database tab
    ProjectForge *forge; // the Forge tab: the project's Forge servers and their sites
    BoardTab *board_tab; // the Board tab: the project's GitHub Projects board
    ProjectFiles *files; // the Files tab: the repository's tree and files
    bool shown;
    Session *runs; size_t run_count; Request *req_runs;   // the project's conversations, for "N runs" on each pull request
    time_t synced_at;
    BoardFilter pull_filter, issue_filter;
    bool hide_later_stacked;
    bool loaded, has_opening;
    BoardFilter opening;
    char *error;
    Request *req;
    Poller poller;
    bool fresh_pending, list_retry_armed, list_retry_read;
    Json *catalog;      // the server's `actions`, for the buttons under each pull request
    Request *req_actions, *req_start;
    bool busy, uncertain, dialog_open;
    int starting_number; char *starting_id;
    char *write_error;
    int merging_number; Request *req_merge;   // the pull request being merged: read for its head first, then merged
    // The Status each linked issue has on its project board, by issue number, from that issue's own read (saved, and
    // shared with the issue and pull request screens). `status_read` holds the ones read this visit; one at a time.
    Json *issue_status, *status_read; Request *req_status;
} PullsScreen;

enum { EDIT_ASSIGN_ME = 1, EDIT_ASSIGNEES, EDIT_LOGIN };

// MARK: - Pull request

enum {
    ACT_OPEN_URL = 1100, ACT_STACK_ITEM, ACT_STACK_TOGGLE, ACT_PR_TAB, ACT_FINDING_TOGGLE, ACT_FINDING_DECISION, ACT_MERGE,
    ACT_START_ACTION, ACT_OPEN_RUN, ACT_CHECK_URL, ACT_COMMIT_URL, ACT_ISSUE_URL, ACT_FINDING_URL, ACT_RELOAD, ACT_SOLVE_FINDINGS, ACT_CONV_URL,
    ACT_DELETE_RUN, ACT_DELETE_SERVED, ACT_WEB_RELOAD, ACT_WEB_BROWSER, ACT_WEB_BACK, ACT_WEB_FORWARD, ACT_RUN_PROFILE, ACT_RUN_RETRY, ACT_PR_PROJECT,
    ACT_EDIT, ACT_EDIT_ASSIGNEES, ACT_EDIT_LABELS, ACT_UPDATE_BRANCH,
    ACT_FILES_BASE = 1200,   // the Files changed tab's own actions, PULL_FILES_ACTIONS of them
};

enum { TIMER_FILES_PAGE = 2, TIMER_RUN_LOG };

enum { TAG_PULL = 1, TAG_FINDINGS, TAG_ROWS, TAG_ACTIONS, TAG_SESSIONS, TAG_START, TAG_MERGE, TAG_DECIDE, TAG_BODY, TAG_CONV, TAG_DELETE_RUN, TAG_RUN, TAG_PROFILES, TAG_RUN_LOG, TAG_ACCESS, TAG_ISSUE, TAG_EDIT, TAG_UPDATE_BRANCH };

enum { PR_TAB_BODY, PR_TAB_CONVERSATION, PR_TAB_SESSIONS, PR_TAB_FILES, PR_TAB_COMMITS, PR_TAB_CHECKS, PR_TAB_FINDINGS, PR_TAB_RUN };

typedef struct { const char *op, *field; } screen_pulls_conv_feeds_type;
extern const screen_pulls_conv_feeds_type screen_pulls_conv_feeds[CONV_FEEDS];
enum { LINKED_ISSUES_MAX = 10 };

typedef struct { Json *items, *incoming; bool read; char *error; Request *req; } ConvFeed;

typedef struct {
    Screen base;
    Project project; int number;
    bool has_stack; StackPosition stack; bool stack_open;   // the overview under the title, as GitHub's stack popover
    bool has_summary; PullSummary summary;
    Json *pr;
    bool has_row; PullSummary row; bool row_read;
    Json *catalog;      // the server's `actions`
    Session *runs; size_t run_count;
    char *deleting_run, *run_error;   // the conversation being deleted from the Sessions tab, and why the last one failed
    Json *findings;
    PullFiles *files;   // the Files changed tab
    ConvFeed conv[CONV_FEEDS];   // the Conversation tab
    char *error, *findings_error, *write_error, *merge_error;
    bool busy, uncertain, merging;
    // An edit of its title, description, labels or assignees, and an update of its branch from its base: `branch_note`
    // says GitHub took the update, which it makes on its own a moment later.
    bool editing, updating_branch;
    char *edit_error, *branch_note;
    Request *req_edit, *req_update_branch;
    bool runs_read;     // the Sessions list was read once
    // The Run tab: opening it serves the pull request (▶ Run) and shows it in an embedded browser laid over the tab's area
    // (`web_rc`, in document coordinates). `run_session` is the session serving it, `run_profile` the run profile it
    // serves, `run_want` the one picked in the tab, `run_asked` the one the request in flight asked for.
    char *run_url, *run_session, *run_profile, *run_want, *run_asked, *serve_error;
    bool run_busy, run_pending;   // pending: waits for the Sessions list, in case a Run already serves this pull request
    char **profiles; size_t profile_count; bool profiles_read;   // the project's run profiles, the default first
    Request *req_run, *req_profiles;
    // The log of the Run under way, from its session's transcript: `log_session` is the session followed (found in the
    // Sessions list while `serve_pull` prepares it).
    char *log_session; RunLog log; Request *req_log;
    WebView *web; RECT web_rc; bool shown; RunAddress address;
    // The Cloudflare Access service token the browser sends to preview hosts, read once before it first opens; without
    // one (an older server, or none configured) the page asks for a sign-in instead.
    char *access_id, *access_secret, *access_suffix; bool access_read; Request *req_access;
    int tab;
    char *body, *body_author;   // the description, from `pull_files` when `pull` leaves it out
    bool body_read;
    char *deciding;
    // The projects of the issues it closes, as GitHub's sidebar shows them: one `issue` read per linked issue in this
    // repository, once per visit and on refresh, kept as [{number, projects, projectsError}] in the sidebar's order.
    // The round keeps the issue numbers it reads, so a list that changes partway starts it again.
    Json *issue_projects, *issue_incoming; bool issues_read; Request *req_issue;
    int issue_round[LINKED_ISSUES_MAX]; size_t issue_round_count;
    int *open_findings; size_t open_finding_count;
    BoardAction *actions; size_t action_count;
    Request *req_pull, *req_findings, *req_rows, *req_actions, *req_sessions, *req_start, *req_merge, *req_decide, *req_body, *req_delete_run;
    Poller poller;
    bool dialog_open;
} PullScreen;

extern const char *const screen_pulls_decision_ids[4];
typedef struct { wchar_t glyph; COLORREF color; char *name, *result; } CheckData;

typedef struct { int passed, failed, pending; } CountsData;

// A column of the pull request screen: the whole width, or one of two side by side when the pane is wide.
typedef struct { int x, w, ix, iw; } Col;

/// Text that wraps a word at a time and mixes fonts, colours and branch chips: the title with its muted number, and the line under it.
typedef struct { char *text; FontId font; COLORREF color; bool chip; int x, y, w; } FlowRun;

typedef struct { FlowRun *runs; size_t count; int line_h; } FlowData;

/// `.timeline-comment-header`: a tinted strip with `author commented · updated …`, a review's verdict glyph first.
typedef struct { char *author, *when; wchar_t glyph; COLORREF glyph_color; } CommentHeadData;

// MARK: - Issue

enum { ACT_ISSUE_OPEN = 1200, ACT_ISSUE_PARENT, ACT_ISSUE_PULL, ACT_ISSUE_START, ACT_ISSUE_CHECKED, ACT_ISSUE_SUB, ACT_ISSUE_RUN, ACT_ISSUE_COPY, ACT_ISSUE_REFRESH, ACT_ISSUE_CLOSE,
       ACT_ISSUE_SUB_LINK, ACT_ISSUE_EVENT, ACT_ISSUE_PROJECT, ACT_ISSUE_MORE, ACT_ISSUE_EDIT, ACT_ISSUE_ASSIGNEES, ACT_ISSUE_LABELS };

// The issue as GitHub has it (`issue`): its body, type, projects, every sub-issue and linked pull request, and its
// timeline a page at a time. The board is still read beside it, on a timer, for the rows of the open sub-issues and
// pull requests, which carry their checks and reviews. On a server without the issue's own read, the board's row is
// all the screen has.
typedef struct {
    Screen base;
    Project project; IssueSummary issue;
    IssueSummary *board_issues; size_t board_issue_count;
    PullSummary *board_pulls; size_t board_pull_count;
    bool board_read, gone;      // gone: the board was read and this issue is not on it
    bool fresh_pending, list_retry_armed, list_retry_read;
    char *load_error, *detail_error;
    Json *detail;                        // the issue in full; NULL until read
    BoardLink *subs; size_t sub_count;   // every sub-issue it lists, closed ones and other repositories' included
    Json *events;                        // the timeline pages read so far, oldest first
    int next_page;                       // the timeline's next page; 0 once it is all read
    bool timeline_read;
    char *timeline_error;
    char *timeline_seen, *timeline_want; // the issue's updatedAt the timeline was read at, and is being read at
    Session *runs; size_t run_count;   // the conversations started on this issue or on a pull request closing it
    bool busy, uncertain;
    char *write_error, *edit_error;   // edit_error is its own, so an edit leaves an uncertain start's notice up
    bool closing, closed;           // closed: this screen closed it; the board no longer lists it
    char *closed_reason;            // completed or not_planned, as the server answered
    bool editing;                   // its title, description, labels or assignees are being saved
    Request *req, *req_board, *req_runs, *req_close, *req_detail, *req_timeline, *req_edit;
    Poller poller;
} IssueScreen;

// The timeline: comments in boxes as GitHub draws them, and every other event a line beside a round badge.

typedef struct { wchar_t glyph; COLORREF color; } EventBadge;

const char *screen_pulls_action_icon(const char *id);
bool screen_pulls_action_prompt(const BoardAction *a, int number, char **input);
BoardAction *screen_pulls_row_actions(const Json *catalog, const PullSummary *pull, int failed_checks, size_t *count);
void screen_pulls_list_retry_stop(Poller *poller, bool *armed);
void screen_pulls_list_retry_arm(Poller *poller, bool *armed, const char *repo);
bool screen_pulls_list_retry_fired(Poller *poller, bool *armed, const char *repo, UINT id, bool paused);
char *screen_pulls_saved_issue_key(const char *repo, int number);
const char *screen_pulls_issue_project_status(const Json *issue);
BoardFilter *screen_pulls_current_filter(PullsScreen *s);
bool screen_pulls_stack_filter_passes(const PullsScreen *s, size_t index);
bool screen_pulls_row_filter_passes(PullsScreen *s, const BoardRow *row, size_t index);
BoardRow *screen_pulls_rows_of(PullsScreen *s, size_t *count);
void screen_pulls_filters_save(PullsScreen *s);
bool screen_pulls_filters_restore(PullsScreen *s);
char *screen_pulls_number_key(int number);
bool screen_pulls_pulls_read(const char *repo, bool fresh, void *owner, RequestDone done, Request **slot);
void screen_pulls_pulls_done(void *owner, Request *req);
size_t screen_pulls_runs_on(PullsScreen *s, int number);
bool screen_pulls_run_active_on(PullsScreen *s, int number);
void screen_pulls_pulls_load(PullsScreen *s, bool fresh);
void screen_pulls_board_start(PullsScreen *s, int number, const char *branch, const BoardAction *action, const char *input);
void screen_pulls_board_merge(PullsScreen *s, size_t index);
const char *screen_pulls_reviewer(PullsScreen *s);
size_t *screen_pulls_review_rows(PullsScreen *s, size_t *count);
void screen_pulls_pulls_layout(Screen *base, Doc *doc);
void screen_pulls_pulls_header(Screen *base, HeaderInfo *info);
void screen_pulls_filter_pick(PullsScreen *s, FilterKind kind, POINT pt);
Screen *pulls_screen_new(const Project *project);
const char *screen_pulls_github_login(void);
bool screen_pulls_github_login_ask(void);
Json *screen_pulls_assignees_edit(HWND hwnd, POINT pt, char *const *assignees, size_t count, int number);
Json *screen_pulls_labels_edit(const PullLabel *labels, size_t count, int number);
Json *screen_pulls_details_edit(const char *what, int number, const char *title, const char *body);
const PullSummary *screen_pulls_board_row(PullScreen *s);
bool screen_pulls_is_open(PullScreen *s);
void screen_pulls_save_pull(PullScreen *s);
void screen_pulls_conv_load(PullScreen *s);
void screen_pulls_conv_cancel(PullScreen *s);
void screen_pulls_issue_projects_load(PullScreen *s);
void screen_pulls_sessions_done_pull(void *owner, Request *req);
void screen_pulls_pull_load(PullScreen *s);
bool screen_pulls_finding_open(PullScreen *s, int i);
void screen_pulls_check_free(void *p);
void screen_pulls_paint_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
void screen_pulls_paint_counts(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
int screen_pulls_section_box(Doc *doc, int w);
Col screen_pulls_col_make(int x, int w);
int screen_pulls_col_box(Doc *doc, Col c);
void screen_pulls_col_gap(Doc *doc, Col c);
int screen_pulls_doc_pill(Doc *doc, int x, int y, const char *text, COLORREF color);
int screen_pulls_doc_stack_pill(Doc *doc, int x, int y, const char *text, bool open);
void screen_pulls_layout_stack_overview(PullScreen *s, Doc *doc, Col c);
void screen_pulls_flow_free(void *p);
FlowData *screen_pulls_flow_new(int line_h);
void screen_pulls_flow_add(FlowData *d, const char *text, FontId font, COLORREF color, bool chip);
int screen_pulls_doc_flow(Doc *doc, int x, int w, int indent, FlowData *d);
int screen_pulls_toolbar_width(Canvas *cv, const ButtonSpec *buttons, size_t count);
const char *screen_pulls_pull_author(PullScreen *s);
const char *screen_pulls_pull_body(PullScreen *s);
void screen_pulls_layout_header(PullScreen *s, Doc *doc, Col c);
bool screen_pulls_conv_read(PullScreen *s);
const char *screen_pulls_run_target(PullScreen *s);
void screen_pulls_layout_tabs(PullScreen *s, Doc *doc, Col c);
int screen_pulls_comment_begin(Doc *doc, Col c, CommentHeadData *h, int action, intptr_t arg, int *ix, int *iw);
void screen_pulls_layout_description(PullScreen *s, Doc *doc, Col c);
void screen_pulls_layout_conversation(PullScreen *s, Doc *doc, Col c);
void screen_pulls_layout_checks(PullScreen *s, Doc *doc, Col c);
void screen_pulls_layout_commit_list(PullScreen *s, Doc *doc, Col c);
size_t screen_pulls_findings_unfixed(PullScreen *s);
size_t screen_pulls_findings_to_fix(PullScreen *s);
bool screen_pulls_solve_findings_offered(PullScreen *s);
void screen_pulls_layout_findings(PullScreen *s, Doc *doc, Col c);
void screen_pulls_paint_delete_run(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
void screen_pulls_layout_runs(PullScreen *s, Doc *doc, Col c);
void screen_pulls_delete_run_done(void *owner, Request *req);
void screen_pulls_side_heading(Doc *doc, Col c, int *count, const char *title);
void screen_pulls_side_edit_button(Doc *doc, Col c, const char *text, int action, bool enabled);
void screen_pulls_side_project(Doc *doc, Col c, const Json *p, const char *from, int action, intptr_t arg);
void screen_pulls_side_projects_none(Doc *doc, Col c, const char *refused);
void screen_pulls_layout_sidebar(PullScreen *s, Doc *doc, Col c);
const char *screen_pulls_run_shown_profile(PullScreen *s);
void screen_pulls_layout_run(PullScreen *s, Doc *doc, int w);
void screen_pulls_run_log_tick(PullScreen *s);
void screen_pulls_run_changed(PullScreen *s);
void screen_pulls_web_changed(void *ctx);
void screen_pulls_run_start(PullScreen *s);
void screen_pulls_run_resume(PullScreen *s);
void screen_pulls_profiles_load(PullScreen *s);
void screen_pulls_run_open(PullScreen *s);
void screen_pulls_run_pick_profile(PullScreen *s, POINT pt);
void screen_pulls_start_action(PullScreen *s, const BoardAction *action, const char *input);
void screen_pulls_merge(PullScreen *s, const char *method);
void screen_pulls_pull_edit(PullScreen *s, Json *fields);
void screen_pulls_update_branch(PullScreen *s);
void screen_pulls_decide(PullScreen *s, const char *key, const char *decision);
void screen_pulls_pull_action(Screen *base, int action, intptr_t arg, POINT pt);
void screen_pulls_pull_refresh(Screen *base);
Screen *pull_detail_screen_new(const Project *project, int number, const StackPosition *stack, const PullSummary *summary);
void screen_pulls_board_open_run(PullsScreen *s, size_t index);
void screen_pulls_issue_subs_free(IssueScreen *s);
void screen_pulls_issue_board_show(IssueScreen *s, const Json *board, bool saved);
void screen_pulls_issue_detail_show(IssueScreen *s, const Json *issue);
void screen_pulls_issue_timeline_show(IssueScreen *s, const Json *result, bool first);
char *screen_pulls_issue_cache_key(const IssueScreen *s, const char *what);
void screen_pulls_issue_timeline_load(IssueScreen *s, int page);
void screen_pulls_issue_runs_done(void *owner, Request *req);
void screen_pulls_issue_board_done(void *owner, Request *req);
void screen_pulls_issue_load(IssueScreen *s, bool fresh);
bool screen_pulls_issue_run_active(const IssueScreen *s);
size_t screen_pulls_issue_open_pulls(const IssueScreen *s);
void screen_pulls_issue_open_link(IssueScreen *s, const BoardLink *link);
void screen_pulls_issue_open_pull(IssueScreen *s, const BoardLink *link);
bool screen_pulls_issue_is_closed(const IssueScreen *s);
bool screen_pulls_issue_editable(const IssueScreen *s);
void screen_pulls_issue_layout_body(IssueScreen *s, Doc *doc, Col c);
void screen_pulls_issue_layout_subs(IssueScreen *s, Doc *doc, Col c);
void screen_pulls_issue_layout_runs(IssueScreen *s, Doc *doc, Col c);
void screen_pulls_paint_event_badge(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
void screen_pulls_flow_strong(FlowData *f, const char *text);
void screen_pulls_flow_muted(FlowData *f, const char *text);
void screen_pulls_flow_ref(FlowData *f, const Json *ref, const char *repo);
void screen_pulls_event_open(IssueScreen *s, const Json *e);
void screen_pulls_issue_layout_main(IssueScreen *s, Doc *doc, Col c);
void screen_pulls_issue_layout_sidebar(IssueScreen *s, Doc *doc, Col c);
void screen_pulls_issue_layout(Screen *base, Doc *doc);
void screen_pulls_issue_header(Screen *base, HeaderInfo *info);
Screen *issue_detail_screen_new(const Project *project, const IssueSummary *issue);

#endif
