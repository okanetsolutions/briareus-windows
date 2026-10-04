// The screens, and the app-level navigation they use.
#ifndef BRIAREUS_SCREENS_H
#define BRIAREUS_SCREENS_H
#include "board.h"
#include "models.h"
#include "pane.h"
#include "store.h"

Screen *pairing_screen_new(void);
Screen *projects_screen_new(void);
Screen *sessions_screen_new(const Project *project);
Screen *placeholder_screen_new(void);
/// The dashboard's opening view: Welcome back, and the composer that starts a session on a project.
Screen *new_session_screen_new(const Project *project, const Project *projects, size_t count);
/// The projects the sidebar lists, for the screens that pick one.
const Project *projects_list(size_t *count);
/// After a conversation was deleted: drops it from the saved list and from the sidebar, which then asks the server again.
void sessions_forget(const char *repo, const char *session_id);
Screen *conversation_screen_new(const Session *session);
/// A session's ⚡ Webhook (an admin token's): arm it, set its caps and the instructions webhook, copy its URLs and keys,
/// and rotate the keys.
Screen *webhook_screen_new(const Session *session);
Screen *pulls_screen_new(const Project *project);
Screen *pull_detail_screen_new(const Project *project, int number, const StackPosition *stack, const PullSummary *summary);
/// The changed files on their own, for the conversation's menu.
Screen *pull_files_screen_new(const Project *project, int number);
/// A pull request's changed files as GitHub's Files changed tab: the tree of paths beside the chosen file's diff, laid out
/// inside a host screen. Its items use `PULL_FILES_ACTIONS` actions from `action_base` up and one timer id for the next page.
typedef struct PullFiles PullFiles;
enum { PULL_FILES_ACTIONS = 8 };
PullFiles *pull_files_new(const Project *project, int number, Screen *host, int action_base, UINT page_timer);
void pull_files_free(PullFiles *files);
/// Reads the list, or the pages still missing; nothing once every page was read.
void pull_files_load(PullFiles *files);
void pull_files_cancel(PullFiles *files);
/// Reads the list again from its first page.
void pull_files_refresh(PullFiles *files);
/// True once a list was read or asked for.
bool pull_files_started(PullFiles *files);
void pull_files_layout(PullFiles *files, Doc *doc, int x, int w);
/// True when the action was one of the component's.
bool pull_files_action(PullFiles *files, int action, intptr_t arg);
/// True when the timer was the component's.
bool pull_files_timer(PullFiles *files, UINT id);
/// The column beside a conversation, as the dashboard's `#pr-panel`: its pull request, commits, reviews and findings once
/// it has one, and its context usage.
Screen *session_panel_screen_new(const Session *session);
/// Whether a session has anything for that column to show.
bool session_panel_wanted(const Session *session);
/// Hands the panel the session's latest record, when the panel is that session's.
void session_panel_update(const Session *session);
/// A session's shared browser: the server's headless Chromium its agent drives, watched as live frames and driven with the
/// mouse and keyboard on the same tabs, with its tabs, an address field, and switching it on and off. It docks as a column
/// beside the conversation, or pops out into a window of its own.
Screen *browser_screen_new(const Session *session);
/// Whether the server has the shared browser and this token may read it.
bool browser_offered(void);
/// The conversation's 🌐 Browser: docks the session's browser beside it (or closes it when it already is), brings its own
/// window forward when it was popped out, and opens it in a window when the main window is too narrow for a column.
void browser_open(const Session *session);
/// Whether a session's browser is docked or in a window of its own.
bool browser_is_open(const char *session_id);
/// The popped-out browser windows close (signing out, quitting) or take a new theme.
void browser_windows_close_all(void);
void browser_windows_themed(void);
/// Runs a session operation (`compact`, `clear`, `rename` with compaction settings) through its open conversation,
/// which shows its progress and errors and reads the session again afterwards.
void conversation_session_op(const char *session_id, const char *operation, Json *extra);
Screen *issue_detail_screen_new(const Project *project, const IssueSummary *issue);
/// The review rounds waiting for a decision across every project, as the dashboard's Findings screen.
Screen *findings_screen_new(void);
/// What every project spent over a window, as the dashboard's 📊 home pane.
Screen *dashboard_screen_new(void);
/// The web apps the sidebar strip opens in the detail pane; the screen's id is "whatsapp" or "slack".
typedef enum { WEB_APP_WHATSAPP, WEB_APP_SLACK, WEB_APP_COUNT } WebApp;
/// WhatsApp Web or Slack, from the sidebar strip's buttons.
Screen *web_app_screen_new(WebApp app);
/// The sidebar's ⚙ Settings, as the dashboard's settings page: the projects, the providers, the database pool, the SSH
/// servers, the Forge accounts and the Slack workspaces, each with ＋ New.
Screen *settings_screen_new(void);
/// ⚙ Settings → Meeting assistant: the OpenAI API key, how the assistant speaks for the user, the virtual microphone,
/// and the two models compared over the meetings recorded on this computer.
Screen *meeting_settings_screen_new(void);
/// A project's SSH sessions tab, laid out inside its board: the project's SSH servers down the left, its open sessions as
/// tabs over a terminal on the right. Its items use `PROJECT_SSH_ACTIONS` actions from `action_base` up.
typedef struct ProjectSsh ProjectSsh;
enum { PROJECT_SSH_ACTIONS = 8 };
ProjectSsh *project_ssh_new(const char *repo, Screen *host, int action_base);
void project_ssh_free(ProjectSsh *p);
/// Whether this token may read the SSH servers (an Admin token, on a server that lists them).
bool project_ssh_offered(void);
/// How many sessions are open on a project's servers.
size_t project_ssh_session_count(const char *repo);
/// Reads the servers once; the refresh reads them again.
void project_ssh_load(ProjectSsh *p);
void project_ssh_refresh(ProjectSsh *p);
/// Lays the tab out from `doc->y` down to the bottom of the pane.
void project_ssh_layout(ProjectSsh *p, Doc *doc, int w);
/// The open session's line under the title, and its Reconnect and Close.
void project_ssh_header(ProjectSsh *p, HeaderInfo *info);
/// Shows the open session's terminal over its area, or hides the project's terminals when `shown` is false.
void project_ssh_place(ProjectSsh *p, const RECT *content, int scroll_y, bool shown);
/// True when the action was the tab's.
bool project_ssh_action(ProjectSsh *p, int action, intptr_t arg, POINT pt);
/// The SSH and SFTP sessions tabs read the servers again, after one was saved or deleted.
void servers_ssh_changed(void);
/// A project's SFTP sessions tab, laid out inside its board: the project's SSH servers down the left, its open SFTP
/// sessions as tabs over the server's file tree on the right, with upload and download. Its items use
/// `PROJECT_SFTP_ACTIONS` actions from `action_base` up.
typedef struct ProjectSftp ProjectSftp;
enum { PROJECT_SFTP_ACTIONS = 16 };
ProjectSftp *project_sftp_new(const char *repo, Screen *host, int action_base);
void project_sftp_free(ProjectSftp *p);
/// Whether this token may read the SSH servers the sessions connect to.
bool project_sftp_offered(void);
/// How many SFTP sessions are open on a project's servers.
size_t project_sftp_session_count(const char *repo);
void project_sftp_load(ProjectSftp *p);
/// Reads the servers, and the folder on show, again.
void project_sftp_refresh(ProjectSftp *p);
/// Lays the tab out from `doc->y` down.
void project_sftp_layout(ProjectSftp *p, Doc *doc, int w);
/// The open session's line under the title, and its Upload, Download, New folder, Home, Reconnect and Close.
void project_sftp_header(ProjectSftp *p, HeaderInfo *info);
/// The tab on show takes files dropped from Explorer; hidden, it stops.
void project_sftp_place(ProjectSftp *p, const RECT *content, int scroll_y, bool shown);
/// True when the action was the tab's.
bool project_sftp_action(ProjectSftp *p, int action, intptr_t arg, POINT pt);
/// A right click: a file's or folder's menu. True when the item was the tab's.
bool project_sftp_context(ProjectSftp *p, int action, intptr_t arg, POINT pt);
void servers_sftp_changed(void);
/// A project's Database tab, laid out inside its board: the project's SSH servers down the left as a tree of their
/// databases and tables, and the table clicked as a grid of its rows on the right, queried with the server's stored
/// database login over SSH. Its items use `PROJECT_DB_ACTIONS` actions from `action_base` up.
typedef struct ProjectDb ProjectDb;
enum { PROJECT_DB_ACTIONS = 4 };
ProjectDb *project_db_new(const char *repo, Screen *host, int action_base);
void project_db_free(ProjectDb *p);
/// Whether this token may read the SSH servers and their database logins (an Admin token, on a server that stores them).
bool project_db_offered(void);
void project_db_load(ProjectDb *p);
void project_db_refresh(ProjectDb *p);
/// Lays the tab out from `doc->y` down to the bottom of the pane.
void project_db_layout(ProjectDb *p, Doc *doc, int w);
/// True when the action was the tab's.
bool project_db_action(ProjectDb *p, int action, intptr_t arg, POINT pt);
/// A project's Forge tab, laid out inside its board: the servers of the Laravel Forge accounts available to the project
/// down the left, and the server picked there with its Forge sites on the right. Its items use `PROJECT_FORGE_ACTIONS`
/// actions from `action_base` up.
typedef struct ProjectForge ProjectForge;
enum { PROJECT_FORGE_ACTIONS = 16 };
ProjectForge *project_forge_new(const char *repo, Screen *host, int action_base);
void project_forge_free(ProjectForge *p);
/// Whether this token may read the Forge accounts and, through them, their servers and sites (an Admin token).
bool project_forge_offered(void);
/// Reads the accounts and their servers once; the refresh reads them, and the sites, again.
void project_forge_load(ProjectForge *p);
void project_forge_refresh(ProjectForge *p);
/// Lays the tab out from `doc->y` down to the bottom of the pane.
void project_forge_layout(ProjectForge *p, Doc *doc, int w);
/// The server on show in the line under the title.
void project_forge_header(ProjectForge *p, HeaderInfo *info);
/// Shows the open site's editors over their area, or hides them when `shown` is false.
void project_forge_place(ProjectForge *p, const RECT *content, int scroll_y, bool shown);
/// An editor's notification, Ctrl+S, and whether the open site's unsaved changes may be dropped: true when handled.
bool project_forge_command(ProjectForge *p, int id, int code);
bool project_forge_key(ProjectForge *p, WPARAM vk, bool ctrl);
bool project_forge_can_leave(ProjectForge *p);
/// One Forge site open inside the Forge tab, in place of its server's sites: its Overview, Deploy script and Environment
/// (.env), the last two editable. `account` is the Forge account the server is read with, `server` the ForgeServer and
/// `site` the ForgeSite as listed. Its items and header buttons use `FORGE_SITE_ACTIONS` actions from `action_base` up.
typedef struct ForgeSite ForgeSite;
enum { FORGE_SITE_ACTIONS = 8 };
ForgeSite *forge_site_new(Screen *host, int action_base, double account, const Json *server, const Json *site);
void forge_site_free(ForgeSite *v);
/// Lays the site out from `doc->y` down, in the column from `x` across `w`.
void forge_site_layout(ForgeSite *v, Doc *doc, int x, int w);
/// The open editor's Save.
void forge_site_header(ForgeSite *v, HeaderInfo *info);
void forge_site_place(ForgeSite *v, const RECT *content, int scroll_y, bool shown);
/// Reads the open tab from Forge again.
void forge_site_refresh(ForgeSite *v);
/// Each true when it was the site's; all take a NULL site.
bool forge_site_action(ForgeSite *v, int action, intptr_t arg);
bool forge_site_command(ForgeSite *v, int id, int code);
bool forge_site_key(ForgeSite *v, WPARAM vk, bool ctrl);
/// True unless unsaved changes are kept when asked.
bool forge_site_can_leave(ForgeSite *v);
/// True when the action was the tab's.
bool project_forge_action(ProjectForge *p, int action, intptr_t arg, POINT pt);
/// A project's Run tab, laid out inside its board: the project's default branch served in a clean workspace with its run
/// commands (`serve_branch`), shown in an embedded browser, its run profile picked in the header. Its items and header
/// buttons use `PROJECT_RUN_ACTIONS` actions from `action_base` up, and its log polls on the host pane's timer `timer`.
typedef struct ProjectRun ProjectRun;
enum { PROJECT_RUN_ACTIONS = 8 };
ProjectRun *project_run_new(const char *repo, Screen *host, int action_base, UINT timer);
void project_run_free(ProjectRun *p);
/// Whether this token may serve a branch, on a server that can.
bool project_run_offered(void);
/// The tab was opened: reads the run profiles once, and serves the branch unless a Run already serves it.
void project_run_open(ProjectRun *p);
/// Lays the tab out from `doc->y` down to the bottom of the pane.
void project_run_layout(ProjectRun *p, Doc *doc, int w);
/// The branch and the served address under the title, and the profile picker, Reload, Open in browser and Delete.
void project_run_header(ProjectRun *p, HeaderInfo *info);
/// Shows the browser over the tab's area, or hides it when `shown` is false.
void project_run_place(ProjectRun *p, const RECT *content, int scroll_y, bool shown);
/// Reloads the page, or serves the branch again when there is none.
void project_run_refresh(ProjectRun *p);
/// True when the timer or action was the tab's.
bool project_run_timer(ProjectRun *p, UINT id);
bool project_run_action(ProjectRun *p, int action, intptr_t arg, POINT pt);
/// One project's settings, the dashboard's project form. `row` is the server's Project (NULL with `defaults` for a new
/// one); `defaults` is what a new one starts from.
Screen *project_settings_screen_new(const Json *row, const Json *defaults);
/// The settings sidebar reads the projects again, after one was saved, cloned or deleted; `select_id` > 0 is highlighted.
void settings_projects_changed(int select_id);
/// One provider's settings, the dashboard's provider form, as tabs. `row` is the server's Provider (NULL with `defaults`
/// for a new one); `defaults` is what a new one starts from.
Screen *provider_settings_screen_new(const Json *row, const Json *defaults);
/// The settings sidebar reads the providers again, after one was saved, cloned, deleted or logged in; `open_first` opens
/// the first one left once read (after a delete).
void settings_providers_changed(bool open_first);
/// One server of the database pool, the dashboard's database server form. `row` is the server's DbServer (NULL with
/// `defaults` for a new one).
Screen *db_server_settings_screen_new(const Json *row, const Json *defaults);
/// The settings sidebar reads the database pool again, after a server was saved or deleted; `open_first` opens the first
/// server left once it is read, unless another settings form is up.
void settings_db_servers_changed(bool open_first);
/// What the settings sidebar last read, for the forms; NULL when it is not up.
const Json *settings_project_rows(void);
const Json *settings_db_server_rows(void);
/// The servers in the pool, which is how many sessions with a database may be open at once.
size_t settings_pool_capacity(void);
/// How many rounds the saved conversations of these projects hold, for the sidebar's count.
size_t findings_waiting(const Project *projects, size_t count);
/// The Findings screen read the conversations again: the sidebar counts once more.
void projects_recount_findings(void);

/// Shows a screen as the detail pane's root, unless one with the same id already is.
void app_show_detail(Screen *screen);
void app_push_detail(Screen *screen);
/// Empties the detail pane after its conversation was deleted, unless a form there keeps its unsaved changes.
void app_clear_detail(void);
Pane *app_sidebar_pane(void);
Pane *app_detail_pane(void);
/// The column on the right of a conversation, the dashboard's pull request panel; NULL takes it away.
Pane *app_panel_pane(void);
void app_set_panel(Screen *screen);
/// The column a session's browser docks in, on the right of its conversation; NULL takes it away.
Pane *app_browser_pane(void);
void app_set_browser(Screen *screen);
/// Whether the main window has room for the browser column beside the detail.
bool app_browser_dockable(void);
/// The docked browser fills the main column, the conversation hidden, until set back.
bool app_browser_expanded(void);
void app_set_browser_expanded(bool expanded);
HWND app_window(void);

/// A confirmation with one continue button; true when confirmed.
bool app_confirm(const char *title, const char *message, const char *continue_label, bool destructive);
/// A choice among several buttons; the chosen index or -1.
int app_choose(const char *title, const char *message, const char *const *choices, size_t count);
void app_alert(const char *title, const char *message);

// MARK: - Shared helpers (common.c)

/// One-shot polling with the dashboard's backoff: exponential up to a minute, or the server's Retry-After.
typedef struct { Pane *pane; UINT timer_id; int base_ms; int failures; double retry_after; bool running; } Poller;
void poller_start(Poller *p, Pane *pane, UINT timer_id, int base_ms);
void poller_stop(Poller *p);
/// After a read finished: schedules the next one, sooner after a failure that says when to retry.
void poller_finished(Poller *p, bool failed, double retry_after);
/// True when this timer is the poller's and a read should go out now.
bool poller_fired(Poller *p, UINT id);
void poller_set_base(Poller *p, int base_ms);

void set_string(char **slot, const char *value);
/// A finding's severity as its pill label (CRIT, HIGH, MED or LOW), with the pill's color in `*color`.
const char *finding_severity_label(const char *severity, COLORREF *color);
/// The decisions a finding can be given, as the API spells them and as the segmented control titles them.
#define FINDING_DECISION_COUNT 3
extern const char *const finding_decision_ids[FINDING_DECISION_COUNT];
extern const char *const finding_decision_titles[FINDING_DECISION_COUNT];
/// The decision's index in `finding_decision_ids`, or -1.
int finding_decision_index(const char *decision);
/// "Status · model", as the rows word it.
char *session_subtitle(const Session *session);
/// A session row: status dot, title and subtitle, clickable. `trailing` keeps that much room at the right for an item laid
/// over the row (a delete button). Advances the doc; returns the row's box.
int doc_session_row(Doc *doc, int x, int w, const Session *session, int action, intptr_t arg, bool selected, COLORREF background, int trailing);
/// A pull request's badges (conflicts, checks, stack, review, draft) as BadgeSpecs. Returns count.
size_t pull_badges(const PullSummary *pull, const StackPosition *stack, BadgeSpec *out, size_t cap, char *stack_text, size_t stack_text_len);
/// Adds label chips for GitHub labels. Returns nothing; advances when there are labels.
void doc_label_chips(Doc *doc, int x, int w, const PullLabel *labels, size_t count, COLORREF background);
/// "@a, @b +2"
char *people(char **logins, size_t count, size_t limit);
/// The linked issue or pull request row under a board row.
void doc_linked_row(Doc *doc, int x, int w, const BoardLink *link, const char *repo, int action, intptr_t arg);
/// The same, with `status` (the issue's Status on its project board) as a chip after its state; NULL or empty for none.
void doc_linked_row_status(Doc *doc, int x, int w, const BoardLink *link, const char *repo, const char *status, int action, intptr_t arg);
/// The row of a pull request on the board; `issue_status`, when given, holds the project Status of each issue it closes.
void doc_pull_row(Doc *doc, int x, int w, const PullSummary *pull, const StackPosition *stack, const char *repo, const char *const *issue_status, int action, intptr_t arg, const ButtonSpec *buttons, size_t button_count, bool running);
/// The row of an issue on the board.
void doc_issue_row(Doc *doc, int x, int w, const IssueSummary *issue, const char *repo, bool nested, int action, intptr_t arg);
/// An epic's bar of closed sub-issues against all of them, with "N/M done" after it.
void doc_epic_progress(Doc *doc, int x, int w, const IssueSummary *issue);
/// A GitHub label's colour for the chips, or the secondary colour.
COLORREF label_color(const PullLabel *label);
/// The glyph for a check conclusion.
wchar_t check_glyph(const char *result, COLORREF *color);
wchar_t review_glyph(ReviewStatus status, COLORREF *color);
/// The glyph a tool event shows, from its name.
wchar_t tool_glyph(const char *kind, const char *name, bool is_error);
/// One tab of a `tabnav` row: a glyph, a title and an optional count, the open one underlined in the accent. Tabs run
/// left to right from `*x`, wrapping to a new line of height `h` past `right`; `*x` and `*y` follow. The open tab is not
/// clickable.
void doc_tab(Doc *doc, int *x, int *y, int left, int right, int h, wchar_t glyph, const char *title, const char *count, bool active, int action, intptr_t arg);
/// "Never" for 0.
int content_left(Pane *pane);

#endif
