#include "screen_projects_internal.h"
#include "ssh_approvals.h"
#include "session_cache.h"

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

static void projects_count(ProjectsScreen *s);
static void projects_show(ProjectsScreen *s, const Json *value);
static void count_done(void *owner, Request *req);
static void projects_done(void *owner, Request *req);
static void projects_load(ProjectsScreen *s);
static void projects_destroy(Screen *base);
static void projects_layout(Screen *base, Doc *doc);
static void projects_header(Screen *base, HeaderInfo *info);
static int projects_footer_height(Screen *base, int width);
static void projects_footer_paint(Screen *base, Canvas *cv, const RECT *rc);
static bool projects_footer_press(Screen *base, POINT pt);
static void projects_footer_drag(Screen *base, POINT pt);
static bool projects_footer_wheel(Screen *base, POINT pt, int delta);
static void projects_footer_click(Screen *base, POINT pt);
static void projects_action(Screen *base, int action, intptr_t arg, POINT pt);
static void projects_timer(Screen *base, UINT id);
static void projects_visible(Screen *base, bool shown);
static void projects_refresh(Screen *base);
static void projects_activated(Screen *base, bool active);

// WhatsApp, dashboard, findings; Mail joins them when the server offers it

size_t screen_projects_g_waiting;
size_t screen_projects_g_status;

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
    screen_projects_g_waiting = findings_waiting(s->projects, s->count) + ssh_approvals_badge();
    screen_projects_g_status = status_waiting(s->projects, s->count);
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
            session_cache_save(s->projects, s->count, items, n);
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

void screen_projects_recount(Pane *sidebar) {
    Screen *root = sidebar ? pane_root(sidebar) : NULL;
    if (!root || root->vt->layout != projects_layout) return;
    ProjectsScreen *s = (ProjectsScreen *)root;
    // Recount sessions against the current project list; disk may still contain an older snapshot.
    projects_count(s);
    pane_relayout(root->pane);
    Screen *top = pane_top(root->pane);
    if (top != root) pane_relayout(top->pane);
}

void projects_recount_findings(void) { screen_projects_recount(app_sidebar_pane()); }

static void projects_destroy(Screen *base) {
    ProjectsScreen *s = (ProjectsScreen *)base;
    request_cancel(&s->req); request_cancel(&s->req_sessions); poller_stop(&s->poller);
    screen_projects_footer_free(&s->footer);
    projects_free(s->projects, s->count); free(s->counts); free(s->busy); free(s->error);
    screen_release(base);
}

static void projects_layout(Screen *base, Doc *doc) {
    ProjectsScreen *s = (ProjectsScreen *)base;
    int w = doc->width;
    screen_projects_sidebar_top(doc, w, pane_selected_id(base->pane));
    if (!store_can_manage()) { doc_text(doc, px(8), w - px(16), "\xF0\x9F\x94\x92 Read-only access", FONT_CAPTION, theme.muted, DT_SINGLELINE | DT_END_ELLIPSIS); doc_space(doc, px(8)); }
    if (s->error) { doc_notice(doc, px(8), w - px(16), s->error); doc_space(doc, px(8)); }
    for (size_t i = 0; i < s->count; i++) {
        const Project *p = &s->projects[i];
        screen_projects_doc_project_row(doc, w, project_title(p), s->counts ? s->counts[i] : 0, s->busy ? s->busy[i] : false, false, true, px(39), ACT_OPEN_PROJECT, (intptr_t)i);
    }
    if (s->loaded && !s->count && !s->error) doc_text(doc, px(8), w - px(16), "No projects yet. Add one in Settings \xE2\x86\x92 Projects on the web dashboard.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    if (!s->loaded) doc_loading(doc, 0, w, "Loading projects\xE2\x80\xA6");
    doc_space(doc, px(8));
}

static void projects_header(Screen *base, HeaderInfo *info) { (void)base; (void)info; }

static int projects_footer_height(Screen *base, int width) { (void)base; return screen_projects_sidebar_footer_height(width); }

static void projects_footer_paint(Screen *base, Canvas *cv, const RECT *rc) { ProjectsScreen *s = (ProjectsScreen *)base; screen_projects_sidebar_footer_paint(cv, rc, &s->footer, false); screen_projects_footer_controls(base, &s->footer); }

static bool projects_footer_press(Screen *base, POINT pt) { return screen_projects_player_press(base->pane, &((ProjectsScreen *)base)->footer, pt); }

static void projects_footer_drag(Screen *base, POINT pt) { screen_projects_player_drag(base->pane, &((ProjectsScreen *)base)->footer, pt); }

static bool projects_footer_wheel(Screen *base, POINT pt, int delta) { return screen_projects_player_wheel(base->pane, &((ProjectsScreen *)base)->footer, pt, delta); }

static void projects_footer_click(Screen *base, POINT pt) {
    ProjectsScreen *s = (ProjectsScreen *)base;
    if (screen_projects_player_click(base->pane, &s->footer, pt)) return;
    if (PtInRect(&s->footer.version_rc, pt)) { updater_menu(base->pane, pt); return; }
    if (PtInRect(&s->footer.ssh_rc, pt)) screen_projects_sidebar_common_action(base->pane, ACT_SSH);
    if (PtInRect(&s->footer.settings_rc, pt)) screen_projects_sidebar_common_action(base->pane, ACT_SETTINGS);
    if (PtInRect(&s->footer.signout_rc, pt)) screen_projects_sidebar_common_action(base->pane, ACT_SIGN_OUT);
}

static void projects_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    ProjectsScreen *s = (ProjectsScreen *)base;
    if (screen_projects_sidebar_common_action(base->pane, action)) return;
    if (action == ACT_NEW) { app_show_detail(new_session_screen_new(NULL, s->projects, s->count)); return; }
    if (action == ACT_OPEN_PROJECT && (size_t)arg < s->count) pane_push(base->pane, sessions_screen_new(&s->projects[arg]));
}

static void projects_timer(Screen *base, UINT id) {
    ProjectsScreen *s = (ProjectsScreen *)base;
    if (poller_fired(&s->poller, id)) projects_load(s);
}

static void projects_visible(Screen *base, bool shown) {
    if (!shown) screen_projects_footer_hide(&((ProjectsScreen *)base)->footer);
    ProjectsScreen *s = (ProjectsScreen *)base;
    if (shown) { projects_count(s); poller_start(&s->poller, base->pane, TIMER_POLL, 30000); }
    else { poller_stop(&s->poller); request_cancel(&s->req); request_cancel(&s->req_sessions); }
}

static void projects_refresh(Screen *base) { ProjectsScreen *s = (ProjectsScreen *)base; request_cancel(&s->req); request_cancel(&s->req_sessions); projects_load(s); }

static void projects_activated(Screen *base, bool active) { if (active) projects_visible(base, true); }

static bool projects_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    return screen_projects_footer_key(&((ProjectsScreen *)base)->footer, vk, ctrl, shift);
}

static const ScreenVTable projects_vt = {
    .destroy = projects_destroy, .layout = projects_layout, .header = projects_header, .action = projects_action,
    .timer = projects_timer, .visible = projects_visible, .refresh = projects_refresh, .activated = projects_activated,
    .key = projects_key, .draw_control = screen_projects_footer_draw, .command = screen_projects_footer_command, .footer_height = projects_footer_height, .footer_paint = projects_footer_paint, .footer_click = projects_footer_click,
    .footer_press = projects_footer_press, .footer_drag = projects_footer_drag, .footer_wheel = projects_footer_wheel,
};

Screen *projects_screen_new(void) {
    ProjectsScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &projects_vt; s->base.id = xstrdup("projects");
    return &s->base;
}

const Project *projects_list(size_t *count) {
    Pane *sidebar = app_sidebar_pane();
    Screen *root = sidebar ? pane_root(sidebar) : NULL;
    if (!root || root->vt->layout != projects_layout) { *count = 0; return NULL; }
    ProjectsScreen *s = (ProjectsScreen *)root;
    *count = s->count;
    return s->projects;
}
