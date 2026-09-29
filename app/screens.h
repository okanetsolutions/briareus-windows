// The screens, and the app-level navigation they use.
#ifndef BRIAREUS_SCREENS_H
#define BRIAREUS_SCREENS_H
#include "board.h"
#include "models.h"
#include "pane.h"
#include "store.h"

Screen *pairing_screen_new(void);
Screen *projects_screen_new(void);
Screen *connection_screen_new(void);
Screen *sessions_screen_new(const Project *project);
Screen *placeholder_screen_new(void);
Screen *conversation_screen_new(const Session *session);
Screen *pulls_screen_new(const Project *project);
Screen *pull_detail_screen_new(const Project *project, int number, const StackPosition *stack, const PullSummary *summary);
Screen *pull_files_screen_new(const Project *project, int number);
Screen *file_diff_screen_new(const PullFile *file);
Screen *issue_detail_screen_new(const Project *project, const IssueSummary *issue);

/// Shows a screen as the detail pane's root, unless one with the same id already is.
void app_show_detail(Screen *screen);
void app_push_detail(Screen *screen);
/// Empties the detail pane after its conversation was deleted.
void app_clear_detail(void);
Pane *app_sidebar_pane(void);
Pane *app_detail_pane(void);
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
/// "Status · model", as the rows word it.
char *session_subtitle(const Session *session);
/// A session row: status dot, title and subtitle, clickable. Advances the doc.
void doc_session_row(Doc *doc, int x, int w, const Session *session, int action, intptr_t arg, bool selected, COLORREF background);
/// A pull request's badges (conflicts, checks, stack, review, draft) as BadgeSpecs. Returns count.
size_t pull_badges(const PullSummary *pull, const StackPosition *stack, BadgeSpec *out, size_t cap, char *stack_text, size_t stack_text_len);
/// Adds label chips for GitHub labels. Returns nothing; advances when there are labels.
void doc_label_chips(Doc *doc, int x, int w, const PullLabel *labels, size_t count, COLORREF background);
/// "@a, @b +2"
char *people(char **logins, size_t count, size_t limit);
/// The linked issue or pull request row under a board row.
void doc_linked_row(Doc *doc, int x, int w, const BoardLink *link, const char *repo, int action, intptr_t arg);
/// The row of a pull request on the board.
void doc_pull_row(Doc *doc, int x, int w, const PullSummary *pull, const StackPosition *stack, const char *repo, int action, intptr_t arg);
/// The row of an issue on the board.
void doc_issue_row(Doc *doc, int x, int w, const IssueSummary *issue, const char *repo, bool nested, int action, intptr_t arg);
/// A GitHub label's colour for the chips, or the secondary colour.
COLORREF label_color(const PullLabel *label);
/// The glyph for a check conclusion.
wchar_t check_glyph(const char *result, COLORREF *color);
wchar_t review_glyph(ReviewStatus status, COLORREF *color);
/// The glyph a tool event shows, from its name.
wchar_t tool_glyph(const char *kind, const char *name, bool is_error);
/// "Never" for 0.
int content_left(Pane *pane);

#endif
