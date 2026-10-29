#ifndef BRIAREUS_SCREEN_PROJECTS_INTERNAL_H
#define BRIAREUS_SCREEN_PROJECTS_INTERNAL_H
#include "dialogs.h"
#include "media.h"
#include "resource.h"
#include "screens.h"
#include "str.h"
#include "updater.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The sidebar, as the dashboard draws it: the ＋ New session strip with WhatsApp, Mail and the 📊 and ⚑ switches, the projects with their
// session counts, and inside a project its conversations; what Spotify plays, ☑ Select, ⚙ Settings and the ⎋ Sign out button along the foot.

// MARK: - What both sidebar screens draw

enum { ACT_NEW = 900, ACT_FINDINGS, ACT_SELECT, ACT_SIGN_OUT, ACT_DASHBOARD, ACT_SETTINGS, ACT_WHATSAPP, ACT_MAIL, ACT_STATUS, ACT_SSH };

enum { STRIP_H = 32, ICON_W = 26, STRIP_GAP = 3, STRIP_ICONS = 4 };

extern size_t screen_projects_g_waiting;
extern size_t screen_projects_g_status;
// review rounds waiting for a decision, the ⚑ badge

typedef enum { MARK_NONE, MARK_WHATSAPP, MARK_MAIL } StripMark;

/// The player above the foot while a media session is open: Spotify's, or whichever app plays, with ⏮ ⏯ ⏭, and under
/// them the Windows volume: the speaker mutes, the slider drags or clicks to a level, and the wheel over the player steps it.
enum { PLAYER_ROW = 36, PLAYER_BUTTON = 26, VOLUME_ROW = 22, VOLUME_STEP = 5 };

typedef struct {
    RECT select_rc, version_rc, ssh_rc, settings_rc, signout_rc, previous_rc, toggle_rc, next_rc;
    RECT player_rc, mute_rc, volume_rc;     // the whole player for the wheel; the slider's track takes clicks around it
    HWND buttons[3], tip;
    int track_left, track_right;
} FooterRects;

/// The foot: `☑ Select` 13px muted and the version (the waiting release's in the accent, opening the updates menu), then
/// at the right the ⚙ Settings icon button and the `⎋ Sign out` button, above a border, under the player.
enum { FOOT_BUTTON = 32 };

// MARK: - Projects

enum { ACT_OPEN_PROJECT = 1000, ACT_ALL_SESSIONS };

enum { TIMER_POLL = 1 };

// MARK: - Sessions

enum { ACT_BACK = 1100, ACT_BOARD, ACT_OPEN_SESSION, ACT_PICK, ACT_SELECT_ALL, ACT_BULK_CLOSE, ACT_BULK_DELETE, ACT_DELETE_ALL };

typedef struct {
    Screen base;
    Project project;
    bool all_projects;
    Session *sessions; size_t count;
    bool loaded;
    char *error;
    Request *req, *req_bulk;
    Poller poller;
    FooterRects footer;
    bool select_mode;
    char **picked; size_t picked_count;
    // a bulk close or delete works through the picked conversations one at a time
    char **queue; size_t queue_count, queue_done; bool queue_delete;
    char *bulk_error;
} SessionsScreen;

bool screen_projects_footer_key(FooterRects *f, WPARAM vk, bool ctrl, bool shift);
void screen_projects_footer_controls(Screen *s, FooterRects *f);
void screen_projects_footer_hide(FooterRects *f);
void screen_projects_footer_free(FooterRects *f);
void screen_projects_footer_command(Screen *s, int id, int code, HWND control);
void screen_projects_sidebar_top(Doc *doc, int w, const char *selected);
bool screen_projects_player_click(Pane *pane, const FooterRects *f, POINT pt);
bool screen_projects_player_press(Pane *pane, const FooterRects *f, POINT pt);
void screen_projects_player_drag(Pane *pane, const FooterRects *f, POINT pt);
bool screen_projects_player_wheel(Pane *pane, const FooterRects *f, POINT pt, int delta);
int screen_projects_sidebar_footer_height(int width);
void screen_projects_sidebar_footer_paint(Canvas *cv, const RECT *rc, FooterRects *out, bool select_on);
bool screen_projects_sidebar_common_action(Pane *pane, int action);
void screen_projects_doc_project_row(Doc *doc, int w, const char *name, int count, bool busy, bool selected, bool chevron, int h, int action, intptr_t arg);
void screen_projects_paint_back_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
void screen_projects_doc_dashboard_session_row(Doc *doc, int w, const Session *s, bool show_repo, bool lit, bool select_mode, bool picked, int action, intptr_t arg);
// Saves an aggregate list and the corresponding lists for cached projects.
void screen_projects_cache_all_sessions(const Session *sessions, size_t count);
void screen_projects_forget_cached_session(const char *repo, const char *id);
void projects_recount_findings(void);
void screen_projects_recount(Pane *sidebar);
Screen *projects_screen_new(void);
const Project *projects_list(size_t *count);
void sessions_forget(const char *repo, const char *id);
void screen_projects_toggle_pick(SessionsScreen *s, const char *id);
void screen_projects_clear_picks(SessionsScreen *s);
void screen_projects_bulk_start(SessionsScreen *s, bool del, bool all);
int screen_projects_sessions_footer_height(Screen *base, int width);
void screen_projects_sessions_footer_paint(Screen *base, Canvas *cv, const RECT *rc);
bool screen_projects_sessions_footer_press(Screen *base, POINT pt);
void screen_projects_sessions_footer_drag(Screen *base, POINT pt);
bool screen_projects_sessions_footer_wheel(Screen *base, POINT pt, int delta);
void screen_projects_sessions_footer_click(Screen *base, POINT pt);
// NULL opens sessions across all projects; a supplied project is copied.
Screen *sessions_screen_new(const Project *project);

#endif
