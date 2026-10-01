// The sidebar, as the dashboard draws it: the ＋ New session strip with the 📊 and ⚑ switches and ⚙ Settings, the projects
// with their session counts, and inside a project its conversations; ☑ Select and ⎋ along the foot.
#include "dialogs.h"
#include "resource.h"
#include "screens.h"
#include "str.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MARK: - What both sidebar screens draw

enum { ACT_NEW = 900, ACT_FINDINGS, ACT_SELECT, ACT_SIGN_OUT, ACT_DASHBOARD, ACT_SETTINGS };
enum { STRIP_H = 32, ICON_W = 32, STRIP_GAP = 6 };

static size_t g_waiting;   // review rounds waiting for a decision, the ⚑ badge

typedef struct { char text[40]; int badge; bool active, wide; } StripData;
static void paint_strip(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    StripData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    COLORREF border = d->active ? theme.accent : hovered ? theme.accent_dim : theme.line;
    fill_round_rect(cv, rc, px(8), theme.raise, border);
    if (d->wide) {
        RECT t = { rc->left + px(8), rc->top, rc->right - px(8), rc->bottom };
        draw_text(cv, d->text, &t, FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        RECT t = *rc;
        draw_text(cv, d->text, &t, FONT_EMOJI, d->active ? theme.accent : theme.ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (d->badge) {
        // `absolute -right-1.5 -top-1.5 min-w-4 rounded-full bg-accent px-1 text-[10px] font-semibold leading-4`
        char n[16]; snprintf(n, sizeof n, "%d", d->badge);
        int tw = text_width(cv, n, FONT_TINY_SEMIBOLD) + px(8);
        if (tw < px(16)) tw = px(16);
        RECT b = { rc->right + px(6) - tw, rc->top - px(6), rc->right + px(6), rc->top - px(6) + px(16) };
        fill_round_rect(cv, &b, px(8), theme.accent, theme.accent);
        draw_text(cv, n, &b, FONT_TINY_SEMIBOLD, theme.on_accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}
static void strip_button(Doc *doc, const RECT *rc, const char *text, bool wide, int badge, bool active, int action) {
    int i = doc_add(doc, rc, paint_strip);
    StripData *d = xcalloc(1, sizeof *d);
    snprintf(d->text, sizeof d->text, "%s", text); d->wide = wide; d->badge = badge; d->active = active;
    Item *it = doc_item(doc, i);
    it->data = d; it->free_data = free; it->action = action; it->hand = true;
}
/// The strip; `selected` is the detail pane's root id, for the 📊 and ⚑ switches' accent.
static void sidebar_top(Doc *doc, int w, const char *selected) {
    doc_space(doc, px(10));
    int y = doc->y, h = px(STRIP_H), iw = px(ICON_W), gap = px(STRIP_GAP);
    int icons_w = iw * 3 + gap * 2;
    RECT nr = { 0, y, w - icons_w - gap, y + h };
    strip_button(doc, &nr, "\xEF\xBC\x8B New session", true, 0, false, ACT_NEW);
    int x = w - icons_w;
    RECT dr = { x, y, x + iw, y + h }; strip_button(doc, &dr, "\xF0\x9F\x93\x8A", false, 0, str_eq(selected, "dashboard"), ACT_DASHBOARD);
    x += iw + gap;
    RECT fr = { x, y, x + iw, y + h }; strip_button(doc, &fr, "\xE2\x9A\x91", false, (int)g_waiting, str_eq(selected, "findings"), ACT_FINDINGS);
    x += iw + gap;
    RECT sr = { x, y, x + iw, y + h }; strip_button(doc, &sr, "\xE2\x9A\x99", false, 0, false, ACT_SETTINGS);
    doc->y = y + h;
    doc_space(doc, px(14));
}

/// The foot: `☑ Select` and `⎋`, 13px muted, above a border.
static int sidebar_footer_height(int width) { (void)width; return px(6) + 1 + px(10) + px(18) + px(2) + px(10); }
typedef struct { RECT select_rc, signout_rc; } FooterRects;
static void sidebar_footer_paint(Canvas *cv, const RECT *rc, FooterRects *out, bool select_on) {
    fill_rect(cv, rc, theme.sidebar);
    int top = rc->top + px(6);
    draw_line(cv, rc->left + px(10), top, rc->right - px(10), top, theme.line);
    int y = top + 1 + px(10), h = px(18);
    int left = rc->left + px(16), right = rc->right - px(16);
    const char *sel = "\xE2\x98\x91 Select", *out_ = "\xE2\x8E\x8B", *version = "v" APP_VERSION_STRING;
    int sw = text_width(cv, sel, FONT_FOOTNOTE), ow = text_width(cv, out_, FONT_FOOTNOTE), vw = text_width(cv, version, FONT_CAPTION2);
    RECT a = { left, y, left + sw, y + h }, c = { right - ow, y, right, y + h };
    RECT v = { (left + right) / 2 - vw / 2, y, (left + right) / 2 + vw / 2 + 1, y + h };
    draw_text(cv, sel, &a, FONT_FOOTNOTE, select_on ? theme.ink : theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(cv, version, &v, FONT_CAPTION2, theme.tertiary, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    draw_text(cv, out_, &c, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    InflateRect(&a, px(4), px(4)); InflateRect(&c, px(4), px(4));
    out->select_rc = a; out->signout_rc = c;
}
static void sign_out(void) {
    if (!app_confirm("Sign out of this dashboard?", "The device token and the saved conversations are removed from this computer. Revoke the token itself in web Settings.", "Sign out", true)) return;
    store_forget();
}
/// The strip's own actions, the same on both screens. True when handled.
static bool sidebar_common_action(Pane *pane, int action) {
    switch (action) {
    case ACT_DASHBOARD: app_show_detail(dashboard_screen_new()); return true;
    case ACT_FINDINGS: app_show_detail(findings_screen_new()); return true;
    // Settings take the sidebar's place, as the dashboard's settings page has a sidebar of its own.
    case ACT_SETTINGS: pane_push(pane, settings_screen_new()); return true;
    case ACT_SIGN_OUT: sign_out(); return true;
    }
    return false;
}

// MARK: - Rows

typedef struct { char *name; int count; bool busy, selected, chevron; } ProjectRowData;
static void project_row_free(void *p) { ProjectRowData *d = p; free(d->name); free(d); }
static void paint_project_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    ProjectRowData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    if (hovered || d->selected) fill_round_rect(cv, rc, px(8), theme.raise, theme.raise);
    int right = rc->right - px(8);
    if (d->chevron) { RECT c = { right - px(6), rc->top, right, rc->bottom }; draw_text(cv, "\xE2\x80\xBA", &c, FONT_CAPTION2, theme.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE); right -= px(6) + px(8); }
    char n[16]; snprintf(n, sizeof n, "%d", d->count);
    int nw = text_width(cv, n, FONT_CAPTION);
    RECT cr = { right - nw, rc->top, right, rc->bottom }; draw_text(cv, n, &cr, FONT_CAPTION, theme.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    right -= nw + px(8);
    if (d->busy) { draw_status_dot(cv, right - px(4), (rc->top + rc->bottom) / 2, "running"); right -= px(7) + px(8); }
    RECT t = { rc->left + px(8), rc->top, right, rc->bottom };
    draw_text(cv, d->name, &t, FONT_SUBHEADLINE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static void doc_project_row(Doc *doc, int w, const char *name, int count, bool busy, bool selected, bool chevron, int h, int action, intptr_t arg) {
    ProjectRowData *d = xcalloc(1, sizeof *d);
    d->name = xstrdup(name); d->count = count; d->busy = busy; d->selected = selected; d->chevron = chevron;
    doc_custom(doc, 0, w, h, paint_project_row, d, project_row_free, action, arg);
}

static void paint_back_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    bool hovered = doc_item_hovered(doc, it);
    if (hovered) fill_round_rect(cv, rc, px(8), theme.raise, theme.raise);
    COLORREF c = hovered ? theme.ink : theme.muted;
    RECT a = { rc->left + px(8), rc->top, rc->left + px(8) + px(8), rc->bottom };
    draw_text(cv, "\xE2\x80\xB9", &a, FONT_FOOTNOTE, c, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT t = { a.right + px(6), rc->top, rc->right, rc->bottom };
    draw_text(cv, "All projects", &t, FONT_CAPTION, c, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

/// A conversation as the dashboard lists it: its mark, title, and a line of provider, branch, state and age.
typedef struct {
    char *title, *provider, *branch, *state, *ago, *pr_state;
    bool lit, select_mode, picked, orchestrator, zeus;
    int meta_h;
} SessionRowData;
static void session_row_free(void *p) { SessionRowData *d = p; free(d->title); free(d->provider); free(d->branch); free(d->state); free(d->ago); free(d->pr_state); free(d); }
static int chip_w(Canvas *cv, const char *text) { return px(5) * 2 + text_width(cv, text, FONT_CAPTION2) + 2; }
/// Lays the metadata chips out at `width`, wrapping as `flex-wrap` does; NULL cv rects only measure. Returns the height.
static int meta_layout(Canvas *cv, SessionRowData *d, int width, RECT *rects) {
    // provider chip, branch chip (at most 45% wide), state, age; `gap-2` between them, 20px lines.
    int lh = px(20), gap = px(8), x = 0, y = 0;
    const char *branch = d->orchestrator ? (d->zeus ? "\xE2\x9A\xA1 zeus" : "\xF0\x9F\xA7\xAD orchestrator") : d->branch;
    int widths[4] = { chip_w(cv, d->provider), branch && *branch ? chip_w(cv, branch) : 0, text_width(cv, d->state, FONT_CAPTION), text_width(cv, d->ago, FONT_CAPTION) };
    if (widths[1] > width * 45 / 100) widths[1] = width * 45 / 100;
    for (int i = 0; i < 4; i++) {
        if (!widths[i]) { SetRectEmpty(&rects[i]); continue; }
        // The state and its age wrap as one, so "waiting" never sits a line above "11h ago".
        int need = widths[i] + (i == 2 && widths[3] ? gap + widths[3] : 0);
        bool joined = i == 3 && !IsRectEmpty(&rects[2]);
        if (x > 0 && !joined && x + need > width) { x = 0; y += lh; }
        RECT r = { x, y, x + widths[i], y + lh }; rects[i] = r;
        x += widths[i] + gap;
    }
    return y + lh;
}
/// GitHub's pull request mark, drawn in 13px: a branch with a commit at each end and the merge ring beside it.
static void draw_pr_mark(Canvas *cv, int x, int y, COLORREF color) {
    int s = px(13);
    int w = px(1) + 1;
    int lx = x + s * 3 / 13, top = y + s * 3 / 13, bottom = y + s * 11 / 13, rx = x + s * 10 / 13;
    draw_thick_line(cv, lx, top, lx, bottom, color, w);
    draw_thick_line(cv, rx, bottom, rx, y + s * 5 / 13, color, w);
    draw_thick_line(cv, rx, y + s * 4 / 13, x + s * 6 / 13, y + s * 4 / 13, color, w);
    int r = s * 2 / 13 + 1;
    fill_circle(cv, lx, top, r, color); fill_circle(cv, lx, bottom, r, color); fill_circle(cv, rx, bottom, r, color);
}
static void paint_session_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    SessionRowData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    if (hovered || d->lit) fill_round_rect(cv, rc, px(8), theme.raise, theme.raise);
    int x = rc->left + px(8), top = rc->top + px(7);
    if (d->select_mode) {
        RECT box = { x, top + px(4), x + px(13), top + px(4) + px(13) };
        fill_round_rect(cv, &box, px(2), d->picked ? theme.accent : theme.field, d->picked ? theme.accent : theme.line_strong);
        if (d->picked) draw_check_mark(cv, &box, theme.on_accent);
        x += px(13) + px(8);
    }
    x += px(8) + px(8);   // the fold gutter and the gap after it
    int right = rc->right - px(8), line_h = px(23);
    // Line one: the mark and the title.
    RECT l1 = { x, top, right, top + line_h };
    int mark_w;
    if (d->pr_state) {
        COLORREF c = str_eq(d->pr_state, "open") ? theme.ok : str_eq(d->pr_state, "merged") ? theme.accent : str_eq(d->pr_state, "closed") ? theme.danger : theme.muted;
        draw_pr_mark(cv, x, top + (line_h - px(13)) / 2, c);
        mark_w = px(13);
    } else { draw_status_dot(cv, x + px(3), top + line_h / 2, d->state); mark_w = px(7); }
    l1.left = x + mark_w + px(7);
    draw_text(cv, d->title, &l1, FONT_SUBHEADLINE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    // Line two: the chips, wrapped as they were measured.
    RECT rects[4];
    meta_layout(cv, d, right - x, rects);
    int my = top + line_h + px(2);
    const char *branch = d->orchestrator ? (d->zeus ? "\xE2\x9A\xA1 zeus" : "\xF0\x9F\xA7\xAD orchestrator") : d->branch;
    const char *texts[4] = { d->provider, branch, d->state, d->ago };
    for (int i = 0; i < 4; i++) {
        if (IsRectEmpty(&rects[i])) continue;
        RECT r = { x + rects[i].left, my + rects[i].top, x + rects[i].right, my + rects[i].bottom };
        if (i < 2) {
            RECT chip = { r.left, r.top + px(1), r.right, r.bottom - px(1) };
            fill_round_rect(cv, &chip, px(4), hovered || d->lit ? theme.raise : theme.sidebar, theme.line);
            RECT t = { chip.left + px(5), chip.top, chip.right - px(5) + 2, chip.bottom };
            draw_text(cv, texts[i], &t, FONT_CAPTION2, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else draw_text(cv, texts[i], &r, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}
/// The dashboard's `sessionState`: an idle conversation with a question up is "waiting".
static const char *session_state(const Session *s) {
    if (str_eq(session_status(s), "idle") && json_bool_is(json_get(s->raw, "awaitingAnswer"), true)) return "waiting";
    return session_status(s);
}
static char *session_age(const Session *s) {
    time_t when;
    if (!board_date_parse(json_str(json_get(s->raw, "createdAt")), &when)) return xstrdup("");
    return format_relative(when);
}
static void doc_dashboard_session_row(Doc *doc, int w, const Session *s, bool lit, bool select_mode, bool picked, int action, intptr_t arg) {
    SessionRowData *d = xcalloc(1, sizeof *d);
    d->title = xstrdup(session_display_title(s));
    d->provider = xstrdup(session_provider(s) ? session_provider(s) : "");
    d->branch = xstrdup(json_str_nonempty(json_get(s->raw, "branch")) ? json_str(json_get(s->raw, "branch")) : "");
    d->state = xstrdup(session_state(s)); d->ago = session_age(s);
    d->orchestrator = json_bool_is(json_get(s->raw, "orchestrator"), true) || json_bool_is(json_get(s->raw, "zeus"), true);
    d->zeus = json_bool_is(json_get(s->raw, "zeus"), true);
    const Json *pr = json_get(s->raw, "prStatus");
    const char *pr_state = json_str(json_get(pr, "state"));
    if (pr_state) d->pr_state = xstrdup(str_eq(pr_state, "open") && json_bool_is(json_get(pr, "draft"), true) ? "draft" : pr_state);
    d->lit = lit; d->select_mode = select_mode; d->picked = picked;
    int x = px(8) + (select_mode ? px(13) + px(8) : 0) + px(16);
    RECT rects[4];
    d->meta_h = meta_layout(doc->cv, d, w - x - px(8), rects);
    int h = px(7) + px(23) + px(2) + d->meta_h + px(7);
    doc_custom(doc, 0, w, h, paint_session_row, d, session_row_free, action, arg);
}

// MARK: - Projects

enum { ACT_OPEN_PROJECT = 1000 };
enum { TIMER_POLL = 1 };

typedef struct {
    Screen base;
    Project *projects; size_t count;
    int *counts; bool *busy;   // per project: how many conversations, and whether one is working
    bool loaded;
    char *error;
    Request *req, *req_sessions;
    Poller poller;
    FooterRects footer;
} ProjectsScreen;
static void projects_layout(Screen *base, Doc *doc);

/// What the saved lists say about each project's conversations, before the server is asked.
static void projects_count(ProjectsScreen *s) {
    free(s->counts); free(s->busy);
    s->counts = xcalloc(s->count, sizeof *s->counts); s->busy = xcalloc(s->count, sizeof *s->busy);
    for (size_t i = 0; i < s->count; i++) {
        char *key = xstrfmt("sessions:%s", s->projects[i].repo);
        Json *list = cache_value(g_store.cache, key);
        free(key);
        if (!list) continue;
        Session *sessions; size_t m;
        if (sessions_parse(list, &sessions, &m)) {
            s->counts[i] = (int)m;
            for (size_t k = 0; k < m; k++) if (session_is_active(&sessions[k])) s->busy[i] = true;
            sessions_free(sessions, m);
        }
        json_free(list);
    }
    g_waiting = findings_waiting(s->projects, s->count);
}
static void projects_show(ProjectsScreen *s, const Json *value) {
    Project *items; size_t n;
    if (!projects_parse(value, &items, &n)) return;
    projects_free(s->projects, s->count);
    s->projects = items; s->count = n; s->loaded = true;
    projects_count(s);
}
static void count_done(void *owner, Request *req) {
    ProjectsScreen *s = owner;
    if (req->ok) {
        // One list for every project the token can see; each project's own rows are saved under its key.
        Session *items; size_t n;
        if (sessions_parse(req->result, &items, &n)) {
            for (size_t p = 0; p < s->count; p++) {
                Json *list = json_array();
                for (size_t k = 0; k < n; k++) if (str_eq(session_repo(&items[k]), s->projects[p].repo)) json_array_push(list, json_clone(items[k].raw));
                char *key = xstrfmt("sessions:%s", s->projects[p].repo);
                cache_store(g_store.cache, list, key);
                json_free(list); free(key);
            }
            sessions_free(items, n);
        }
    }
    projects_count(s);
    poller_finished(&s->poller, !req->ok, req->error.retry_after);
    pane_relayout(s->base.pane);
}
static void projects_done(void *owner, Request *req) {
    ProjectsScreen *s = owner;
    if (req->ok) {
        projects_show(s, req->result);
        set_string(&s->error, NULL);
        Json *list = projects_json(s->projects, s->count);
        cache_store(g_store.cache, list, "projects");
        json_free(list);
        pane_relayout(s->base.pane);
        // Every project's conversations, for the counts and the dots.
        if (store_supports("sessions")) { store_call("sessions", json_object(), 0, s, count_done, 0, &s->req_sessions); return; }
    } else {
        request_error_into(&s->error, req);
        s->loaded = true;
    }
    poller_finished(&s->poller, !req->ok, req->error.retry_after);
    pane_relayout(s->base.pane);
}
static void projects_load(ProjectsScreen *s) {
    if (!s->loaded) {
        Json *saved = cache_value(g_store.cache, "projects");
        if (saved) { projects_show(s, saved); json_free(saved); pane_relayout(s->base.pane); }
    }
    if (s->req || s->req_sessions) return;
    store_call("projects", json_object(), 0, s, projects_done, 0, &s->req);
}
void projects_recount_findings(void) {
    Screen *root = pane_root(app_sidebar_pane());
    if (!root || root->vt->layout != projects_layout) return;
    ProjectsScreen *s = (ProjectsScreen *)root;
    projects_count(s);
    pane_relayout(root->pane);
    Screen *top = pane_top(root->pane);
    if (top != root) pane_relayout(top->pane);
}

static void projects_destroy(Screen *base) {
    ProjectsScreen *s = (ProjectsScreen *)base;
    request_cancel(&s->req); request_cancel(&s->req_sessions); poller_stop(&s->poller);
    projects_free(s->projects, s->count); free(s->counts); free(s->busy); free(s->error);
    screen_release(base);
}
static void projects_layout(Screen *base, Doc *doc) {
    ProjectsScreen *s = (ProjectsScreen *)base;
    int w = doc->width;
    sidebar_top(doc, w, pane_selected_id(base->pane));
    if (!store_can_manage()) { doc_text(doc, px(8), w - px(16), "\xF0\x9F\x94\x92 Read-only access", FONT_CAPTION, theme.muted, DT_SINGLELINE | DT_END_ELLIPSIS); doc_space(doc, px(8)); }
    if (s->error) { doc_notice(doc, px(8), w - px(16), s->error); doc_space(doc, px(8)); }
    for (size_t i = 0; i < s->count; i++) {
        const Project *p = &s->projects[i];
        doc_project_row(doc, w, project_title(p), s->counts ? s->counts[i] : 0, s->busy ? s->busy[i] : false, false, true, px(39), ACT_OPEN_PROJECT, (intptr_t)i);
    }
    if (s->loaded && !s->count && !s->error) doc_text(doc, px(8), w - px(16), "No projects yet. Add one in Settings \xE2\x86\x92 Projects on the web dashboard.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    if (!s->loaded) doc_loading(doc, 0, w, "Loading projects\xE2\x80\xA6");
    doc_space(doc, px(8));
}
static void projects_header(Screen *base, HeaderInfo *info) { (void)base; (void)info; }
static int projects_footer_height(Screen *base, int width) { (void)base; return sidebar_footer_height(width); }
static void projects_footer_paint(Screen *base, Canvas *cv, const RECT *rc) { ProjectsScreen *s = (ProjectsScreen *)base; sidebar_footer_paint(cv, rc, &s->footer, false); }
static void projects_footer_click(Screen *base, POINT pt) {
    ProjectsScreen *s = (ProjectsScreen *)base;
    if (PtInRect(&s->footer.signout_rc, pt)) sidebar_common_action(base->pane, ACT_SIGN_OUT);
}
static void projects_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    ProjectsScreen *s = (ProjectsScreen *)base;
    if (sidebar_common_action(base->pane, action)) return;
    if (action == ACT_NEW) { app_show_detail(new_session_screen_new(s->count ? &s->projects[0] : NULL, s->projects, s->count)); return; }
    if (action == ACT_OPEN_PROJECT && (size_t)arg < s->count) pane_push(base->pane, sessions_screen_new(&s->projects[arg]));
}
static void projects_timer(Screen *base, UINT id) {
    ProjectsScreen *s = (ProjectsScreen *)base;
    if (poller_fired(&s->poller, id)) projects_load(s);
}
static void projects_visible(Screen *base, bool shown) {
    ProjectsScreen *s = (ProjectsScreen *)base;
    if (shown) { projects_count(s); poller_start(&s->poller, base->pane, TIMER_POLL, 30000); }
    else { poller_stop(&s->poller); request_cancel(&s->req); request_cancel(&s->req_sessions); }
}
static void projects_refresh(Screen *base) { ProjectsScreen *s = (ProjectsScreen *)base; request_cancel(&s->req); request_cancel(&s->req_sessions); projects_load(s); }
static void projects_activated(Screen *base, bool active) { if (active) projects_visible(base, true); }

static const ScreenVTable projects_vt = {
    .destroy = projects_destroy, .layout = projects_layout, .header = projects_header, .action = projects_action,
    .timer = projects_timer, .visible = projects_visible, .refresh = projects_refresh, .activated = projects_activated,
    .footer_height = projects_footer_height, .footer_paint = projects_footer_paint, .footer_click = projects_footer_click,
};
Screen *projects_screen_new(void) {
    ProjectsScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &projects_vt; s->base.id = xstrdup("projects");
    return &s->base;
}
const Project *projects_list(size_t *count) {
    Screen *root = pane_root(app_sidebar_pane());
    if (!root || root->vt->layout != projects_layout) { *count = 0; return NULL; }
    ProjectsScreen *s = (ProjectsScreen *)root;
    *count = s->count;
    return s->projects;
}

// MARK: - Sessions

enum { ACT_BACK = 1100, ACT_BOARD, ACT_OPEN_SESSION, ACT_PICK, ACT_SELECT_ALL, ACT_BULK_CLOSE, ACT_BULK_DELETE, ACT_DELETE_ALL };

typedef struct {
    Screen base;
    Project project;
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

static void sessions_layout(Screen *base, Doc *doc);
static char *sessions_key(SessionsScreen *s) { return xstrfmt("sessions:%s", s->project.repo); }

static void sessions_show(SessionsScreen *s, const Json *value, bool from_server) {
    Session *items; size_t n;
    if (!sessions_parse(value, &items, &n)) return;
    if (from_server) {
        // Transcripts of conversations that are gone go with them.
        for (size_t i = 0; i < s->count; i++) {
            bool kept = false;
            for (size_t k = 0; k < n && !kept; k++) kept = str_eq(session_id(&s->sessions[i]), session_id(&items[k]));
            if (!kept) { char *key = xstrfmt("transcript:%s", session_id(&s->sessions[i])); cache_remove(g_store.cache, key); free(key); }
        }
    }
    sessions_free(s->sessions, s->count);
    s->sessions = items; s->count = n; s->loaded = true;
}
static void sessions_done(void *owner, Request *req) {
    SessionsScreen *s = owner;
    if (req->ok) {
        sessions_show(s, req->result, true);
        set_string(&s->error, NULL);
        char *key = sessions_key(s);
        Json *list = sessions_json(s->sessions, s->count);
        cache_store(g_store.cache, list, key);
        json_free(list); free(key);
        projects_recount_findings();
    } else {
        request_error_into(&s->error, req);
        s->loaded = true;
    }
    poller_finished(&s->poller, !req->ok, req->error.retry_after);
    pane_relayout(s->base.pane);
}
static void sessions_load(SessionsScreen *s) {
    if (!s->loaded) {
        char *key = sessions_key(s);
        Json *saved = cache_value(g_store.cache, key);
        if (saved) { sessions_show(s, saved, false); json_free(saved); pane_relayout(s->base.pane); }
        free(key);
    }
    if (s->req) return;
    Json *args = json_object(); json_set_str(args, "repo", s->project.repo);
    store_call("sessions", args, 0, s, sessions_done, 0, &s->req);
}

void sessions_forget(const char *repo, const char *id) {
    if (str_empty(repo) || str_empty(id)) return;
    // The saved list, so a restart does not bring the conversation back.
    char *key = xstrfmt("sessions:%s", repo);
    Json *saved = cache_value(g_store.cache, key);
    if (saved) {
        Session *items; size_t n;
        if (sessions_parse(saved, &items, &n)) {
            Json *list = json_array();
            for (size_t i = 0; i < n; i++) if (!str_eq(session_id(&items[i]), id)) json_array_push(list, json_clone(items[i].raw));
            cache_store(g_store.cache, list, key);
            json_free(list); sessions_free(items, n);
        }
        json_free(saved);
    }
    free(key);
    char *transcript = xstrfmt("transcript:%s", id); cache_remove(g_store.cache, transcript); free(transcript);
    // The list on screen, right away rather than at its next poll.
    Screen *top = pane_top(app_sidebar_pane());
    if (!top || top->vt->layout != sessions_layout) return;
    SessionsScreen *s = (SessionsScreen *)top;
    if (!str_eq(s->project.repo, repo)) return;
    for (size_t i = 0; i < s->count; i++) {
        if (!str_eq(session_id(&s->sessions[i]), id)) continue;
        session_free(&s->sessions[i]);
        memmove(&s->sessions[i], &s->sessions[i + 1], (s->count - i - 1) * sizeof *s->sessions);
        s->count--;
        break;
    }
    pane_relayout(s->base.pane);
    // And the server's word on it, in case the list changed in other ways too.
    request_cancel(&s->req); sessions_load(s);
}

static bool is_picked(SessionsScreen *s, const char *id) { for (size_t i = 0; i < s->picked_count; i++) if (str_eq(s->picked[i], id)) return true; return false; }
static void toggle_pick(SessionsScreen *s, const char *id) {
    for (size_t i = 0; i < s->picked_count; i++) if (str_eq(s->picked[i], id)) { free(s->picked[i]); s->picked[i] = s->picked[--s->picked_count]; return; }
    s->picked = xrealloc(s->picked, (s->picked_count + 1) * sizeof *s->picked);
    s->picked[s->picked_count++] = xstrdup(id);
}
static void clear_picks(SessionsScreen *s) { str_array_free(s->picked, s->picked_count); s->picked = NULL; s->picked_count = 0; }
/// Open = still holding a workspace and a database, the only kind Close has anything to release.
static bool session_open(const Session *ses) { const char *st = session_status(ses); return str_eq(st, "queued") || str_eq(st, "preparing") || str_eq(st, "running") || str_eq(st, "idle"); }

static void bulk_next(SessionsScreen *s);
static void bulk_done(void *owner, Request *req) {
    SessionsScreen *s = owner;
    if (!req->ok) { request_error_into(&s->bulk_error, req); }
    else if (s->queue_delete) sessions_forget(s->project.repo, json_str(json_get(req->args, "sessionId")));
    s->queue_done++;
    bulk_next(s);
}
static void bulk_next(SessionsScreen *s) {
    if (s->queue_done >= s->queue_count) {
        str_array_free(s->queue, s->queue_count); s->queue = NULL; s->queue_count = s->queue_done = 0;
        clear_picks(s);
        request_cancel(&s->req); sessions_load(s);
        pane_relayout(s->base.pane); pane_footer_changed(s->base.pane);
        return;
    }
    Json *args = json_object(); json_set_str(args, "sessionId", s->queue[s->queue_done]);
    store_call(s->queue_delete ? "delete" : "close", args, 0, s, bulk_done, 0, &s->req_bulk);
}
static void bulk_start(SessionsScreen *s, bool del, bool all) {
    if (s->req_bulk) return;
    size_t n = 0;
    char **ids = xcalloc(s->count, sizeof *ids);
    for (size_t i = 0; i < s->count; i++) {
        const Session *ses = &s->sessions[i];
        if (!all && !is_picked(s, session_id(ses))) continue;
        if (!del && !session_open(ses)) continue;
        ids[n++] = xstrdup(session_id(ses));
    }
    if (!n) { str_array_free(ids, n); return; }
    char *title = del ? xstrfmt("Delete %zu conversation%s and their logs?", n, n == 1 ? "" : "s") : xstrfmt("Close %zu session%s?", n, n == 1 ? "" : "s");
    bool ok = app_confirm(title, del ? "This cannot be undone." : "Each one releases its workspace and database; the conversation stays readable.", del ? "Delete" : "Close", del);
    free(title);
    if (!ok) { str_array_free(ids, n); return; }
    set_string(&s->bulk_error, NULL);
    s->queue = ids; s->queue_count = n; s->queue_done = 0; s->queue_delete = del;
    bulk_next(s);
}

static void sessions_destroy(Screen *base) {
    SessionsScreen *s = (SessionsScreen *)base;
    request_cancel(&s->req); request_cancel(&s->req_bulk); poller_stop(&s->poller);
    sessions_free(s->sessions, s->count); project_free(&s->project); free(s->error); free(s->bulk_error);
    clear_picks(s); str_array_free(s->queue, s->queue_count);
    screen_release(base);
}

static void sessions_layout(Screen *base, Doc *doc) {
    SessionsScreen *s = (SessionsScreen *)base;
    int w = doc->width;
    const char *selected = pane_selected_id(base->pane);
    sidebar_top(doc, w, selected);
    // `‹ All projects`, the project itself (its pull requests), then its conversations.
    doc_custom(doc, 0, w, px(30), paint_back_row, NULL, NULL, ACT_BACK, 0);
    doc_space(doc, px(2));
    char *board_id = xstrfmt("pulls:%s", s->project.repo);
    doc_project_row(doc, w, project_title(&s->project), (int)s->count, false, str_eq(selected, board_id), false, px(35), ACT_BOARD, 0);
    free(board_id);
    doc_space(doc, px(4));
    if (s->error) { doc_notice(doc, px(8), w - px(16), s->error); doc_space(doc, px(8)); }
    for (size_t i = 0; i < s->count; i++) {
        const Session *ses = &s->sessions[i];
        char *id = xstrfmt("conversation:%s", session_id(ses));
        bool lit = s->select_mode ? is_picked(s, session_id(ses)) : str_eq(selected, id);
        doc_dashboard_session_row(doc, w, ses, lit, s->select_mode, is_picked(s, session_id(ses)), s->select_mode ? ACT_PICK : ACT_OPEN_SESSION, (intptr_t)i);
        free(id);
    }
    if (s->loaded && !s->count) doc_text(doc, px(8), w - px(16), "No conversations here yet.", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
    if (!s->loaded) doc_loading(doc, 0, w, "Loading\xE2\x80\xA6");
    doc_space(doc, px(8));
}
static void sessions_header(Screen *base, HeaderInfo *info) { (void)base; (void)info; }

/// The bulk bar above the foot while ☑ Select is on: the count, Select all, ⏻ Close and 🗑 Delete, 🗑 Delete all.
static int bulk_bar_height(SessionsScreen *s) { return s->select_mode ? px(6) + 1 + px(10) + px(18) + px(6) + px(30) + px(6) + px(18) + px(10) : 0; }
static int sessions_footer_height(Screen *base, int width) { SessionsScreen *s = (SessionsScreen *)base; return sidebar_footer_height(width) + bulk_bar_height(s); }
typedef struct { RECT all, close_, delete_, delete_all; } BulkRects;
static BulkRects g_bulk;
static void sessions_footer_paint(Screen *base, Canvas *cv, const RECT *rc) {
    SessionsScreen *s = (SessionsScreen *)base;
    fill_rect(cv, rc, theme.sidebar);
    int y = rc->top;
    memset(&g_bulk, 0, sizeof g_bulk);
    if (s->select_mode) {
        int left = rc->left + px(16), right = rc->right - px(16);
        y += px(6); draw_line(cv, rc->left + px(10), y, rc->right - px(10), y, theme.line); y += 1 + px(10);
        char *count = xstrfmt("%zu selected", s->picked_count);
        RECT cr = { left, y, right - px(70), y + px(18) }; draw_text(cv, count, &cr, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS); free(count);
        int aw = text_width(cv, "Select all", FONT_FOOTNOTE);
        RECT ar = { right - aw, y, right, y + px(18) }; draw_text(cv, "Select all", &ar, FONT_FOOTNOTE, s->count ? theme.muted : blend(theme.muted, theme.sidebar, 0.5), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        g_bulk.all = ar;
        y += px(18) + px(6);
        int bw = (right - left - px(6)) / 2, bh = px(30);
        bool can = s->picked_count && !s->req_bulk;
        RECT c1 = { left, y, left + bw, y + bh }, c2 = { left + bw + px(6), y, right, y + bh };
        fill_round_rect(cv, &c1, px(7), theme.raise, theme.line); draw_text(cv, s->req_bulk && !s->queue_delete ? "Closing\xE2\x80\xA6" : "\xE2\x8F\xBB Close", &c1, FONT_CAPTION, can ? theme.ink : theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        fill_round_rect(cv, &c2, px(7), theme.raise, theme.line); draw_text(cv, s->req_bulk && s->queue_delete ? "Deleting\xE2\x80\xA6" : "\xF0\x9F\x97\x91 Delete", &c2, FONT_CAPTION, can ? theme.ink : theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        g_bulk.close_ = c1; g_bulk.delete_ = c2;
        y += bh + px(6);
        RECT dr = { left, y, right, y + px(18) }; draw_text(cv, "\xF0\x9F\x97\x91 Delete all", &dr, FONT_CAPTION, s->count && !s->req_bulk ? theme.muted : blend(theme.muted, theme.sidebar, 0.5), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        g_bulk.delete_all = dr;
        y += px(18) + px(10);
        if (s->bulk_error) { RECT er = { left, y - px(8), right, y + px(6) }; draw_text(cv, s->bulk_error, &er, FONT_CAPTION2, theme.danger, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS); }
    }
    RECT foot = { rc->left, y, rc->right, rc->bottom };
    sidebar_footer_paint(cv, &foot, &s->footer, s->select_mode);
}
static void sessions_footer_click(Screen *base, POINT pt) {
    SessionsScreen *s = (SessionsScreen *)base;
    if (PtInRect(&s->footer.select_rc, pt)) { s->select_mode = !s->select_mode; if (!s->select_mode) clear_picks(s); pane_footer_changed(base->pane); return; }
    if (PtInRect(&s->footer.signout_rc, pt)) { sidebar_common_action(base->pane, ACT_SIGN_OUT); return; }
    if (!s->select_mode) return;
    if (PtInRect(&g_bulk.all, pt)) { clear_picks(s); for (size_t i = 0; i < s->count; i++) toggle_pick(s, session_id(&s->sessions[i])); pane_footer_changed(base->pane); }
    else if (PtInRect(&g_bulk.close_, pt)) bulk_start(s, false, false);
    else if (PtInRect(&g_bulk.delete_, pt)) bulk_start(s, true, false);
    else if (PtInRect(&g_bulk.delete_all, pt)) bulk_start(s, true, true);
}
static void sessions_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    SessionsScreen *s = (SessionsScreen *)base;
    if (sidebar_common_action(base->pane, action)) return;
    switch (action) {
    case ACT_BACK: pane_pop(base->pane); break;
    case ACT_BOARD: app_show_detail(pulls_screen_new(&s->project)); break;
    case ACT_NEW: { size_t n; const Project *all = projects_list(&n); app_show_detail(new_session_screen_new(&s->project, all, n)); break; }
    case ACT_OPEN_SESSION:
        if ((size_t)arg >= s->count) break;
        // Ctrl-clicking a row turns ☑ Select on with that row ticked, as the dashboard does.
        if (GetKeyState(VK_CONTROL) & 0x8000) { s->select_mode = true; toggle_pick(s, session_id(&s->sessions[arg])); pane_footer_changed(base->pane); break; }
        app_show_detail(conversation_screen_new(&s->sessions[arg]));
        break;
    case ACT_PICK: if ((size_t)arg < s->count) { toggle_pick(s, session_id(&s->sessions[arg])); pane_footer_changed(base->pane); } break;
    }
}
static void sessions_timer(Screen *base, UINT id) {
    SessionsScreen *s = (SessionsScreen *)base;
    if (poller_fired(&s->poller, id)) sessions_load(s);
}
static void sessions_visible(Screen *base, bool shown) {
    SessionsScreen *s = (SessionsScreen *)base;
    if (shown) poller_start(&s->poller, base->pane, TIMER_POLL, 7000); else { poller_stop(&s->poller); request_cancel(&s->req); }
}
static void sessions_refresh(Screen *base) { SessionsScreen *s = (SessionsScreen *)base; request_cancel(&s->req); sessions_load(s); }
static void sessions_activated(Screen *base, bool active) { if (active) { SessionsScreen *s = (SessionsScreen *)base; poller_start(&s->poller, base->pane, TIMER_POLL, 7000); } }
static bool sessions_key_press(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)ctrl; (void)shift;
    SessionsScreen *s = (SessionsScreen *)base;
    if (vk == VK_ESCAPE && s->select_mode) { s->select_mode = false; clear_picks(s); pane_footer_changed(base->pane); return true; }
    return false;
}

static const ScreenVTable sessions_vt = {
    .destroy = sessions_destroy, .layout = sessions_layout, .header = sessions_header, .action = sessions_action,
    .timer = sessions_timer, .visible = sessions_visible, .refresh = sessions_refresh, .activated = sessions_activated,
    .footer_height = sessions_footer_height, .footer_paint = sessions_footer_paint, .footer_click = sessions_footer_click, .key = sessions_key_press,
};
Screen *sessions_screen_new(const Project *project) {
    SessionsScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &sessions_vt; s->base.id = xstrfmt("sessions:%s", project->repo);
    project_copy(&s->project, project);
    return &s->base;
}
