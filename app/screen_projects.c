// The sidebar: the projects the device may see, and one project's conversations.
#include "dialogs.h"
#include "screens.h"
#include "str.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MARK: - Projects

enum { ACT_CONNECTION = 1000, ACT_OPEN_PROJECT };
enum { TIMER_POLL = 1 };

typedef struct {
    Screen base;
    Project *projects; size_t count;
    bool loaded;
    char *error;
    Request *req;
    Poller poller;
} ProjectsScreen;

static void projects_show(ProjectsScreen *s, const Json *value) {
    Project *items; size_t n;
    if (!projects_parse(value, &items, &n)) return;
    projects_free(s->projects, s->count);
    s->projects = items; s->count = n; s->loaded = true;
}

static void projects_done(void *owner, Request *req) {
    ProjectsScreen *s = owner;
    if (req->ok) {
        projects_show(s, req->result);
        set_string(&s->error, NULL);
        Json *list = projects_json(s->projects, s->count);
        cache_store(g_store.cache, list, "projects");
        json_free(list);
    } else {
        char *text = request_error_text(req); set_string(&s->error, text); free(text);
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
    if (s->req) return;
    store_call("projects", json_object(), 0, s, projects_done, 0, &s->req);
}

static void projects_destroy(Screen *base) {
    ProjectsScreen *s = (ProjectsScreen *)base;
    request_cancel(&s->req); poller_stop(&s->poller);
    projects_free(s->projects, s->count); free(s->error);
    screen_release(base);
}
static void projects_layout(Screen *base, Doc *doc) {
    ProjectsScreen *s = (ProjectsScreen *)base;
    int w = doc->width;
    doc_space(doc, px(8));
    if (!store_can_manage()) { doc_label(doc, px(4), w - px(8), 0xE7B3, "Read-only access", FONT_SUBHEADLINE, theme.secondary); doc_space(doc, px(8)); }
    if (s->error) { doc_notice(doc, px(4), w - px(8), s->error); doc_space(doc, px(8)); }
    for (size_t i = 0; i < s->count; i++) {
        const Project *p = &s->projects[i];
        int box = doc_box_begin(doc, 0, w, px(10), theme.elevated, theme.border, px(10));
        int left = px(12), inner = w - px(24);
        doc_text(doc, left, inner, project_title(p), FONT_BODY_MEDIUM, theme.text, DT_SINGLELINE | DT_END_ELLIPSIS);
        if (!str_eq(project_title(p), p->repo)) { doc_space(doc, px(2)); doc_text(doc, left, inner, p->repo, FONT_CAPTION, theme.secondary, DT_SINGLELINE | DT_END_ELLIPSIS); }
        doc_box_end(doc, box, px(10));
        doc_box_action(doc, box, ACT_OPEN_PROJECT, (intptr_t)i);
        doc_space(doc, px(6));
    }
    if (s->loaded && !s->count && !s->error) doc_empty_state(doc, 0, w, 0xE8B7, "No projects", "Grant this device access to a project in web Settings.");
    if (!s->loaded) doc_loading(doc, 0, w, "Loading projects\xE2\x80\xA6");
}
static void projects_header(Screen *base, HeaderInfo *info) {
    (void)base;
    snprintf(info->title, sizeof info->title, "Projects");
    info->large = true;
    info->buttons[0].glyph = 0xE968; info->buttons[0].action = ACT_CONNECTION; info->buttons[0].enabled = true; info->buttons[0].tip = "Connection";
    info->button_count = 1;
}
static void projects_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    ProjectsScreen *s = (ProjectsScreen *)base;
    if (action == ACT_CONNECTION) { pane_push(base->pane, connection_screen_new()); return; }
    if (action == ACT_OPEN_PROJECT && (size_t)arg < s->count) pane_push(base->pane, sessions_screen_new(&s->projects[arg]));
}
static void projects_timer(Screen *base, UINT id) {
    ProjectsScreen *s = (ProjectsScreen *)base;
    if (poller_fired(&s->poller, id)) projects_load(s);
}
static void projects_visible(Screen *base, bool shown) {
    ProjectsScreen *s = (ProjectsScreen *)base;
    if (shown) poller_start(&s->poller, base->pane, TIMER_POLL, 30000); else { poller_stop(&s->poller); request_cancel(&s->req); }
}
static void projects_refresh(Screen *base) { ProjectsScreen *s = (ProjectsScreen *)base; request_cancel(&s->req); projects_load(s); }
static void projects_activated(Screen *base, bool active) { if (active) projects_visible(base, true); }

static const ScreenVTable projects_vt = {
    .destroy = projects_destroy, .layout = projects_layout, .header = projects_header, .action = projects_action,
    .timer = projects_timer, .visible = projects_visible, .refresh = projects_refresh, .activated = projects_activated,
};
Screen *projects_screen_new(void) {
    ProjectsScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &projects_vt; s->base.id = xstrdup("projects");
    return &s->base;
}

// MARK: - Sessions

enum { ACT_PULLS = 1100, ACT_OPEN_SESSION, ACT_TOGGLE_CLOSED, ACT_NEW };
enum { ID_SEARCH = 201 };

typedef struct {
    Screen base;
    Project project;
    Session *sessions; size_t count;
    bool loaded, show_closed, creating;
    char *error;
    HWND search; RECT search_rc; bool search_placed;
    char *query;
    Request *req;
    Poller poller;
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
    } else {
        char *text = request_error_text(req); set_string(&s->error, text); free(text);
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

static void sessions_destroy(Screen *base) {
    SessionsScreen *s = (SessionsScreen *)base;
    request_cancel(&s->req); poller_stop(&s->poller);
    if (s->search) DestroyWindow(s->search);
    sessions_free(s->sessions, s->count); project_free(&s->project); free(s->error); free(s->query);
    screen_release(base);
}

static bool session_shown(SessionsScreen *s, const Session *session) {
    if (!s->show_closed && str_eq(session_status(session), "closed")) return false;
    return str_empty(s->query) || str_icontains(session_display_title(session), s->query);
}
static bool is_selected(SessionsScreen *s, const char *id) {
    const char *selected = pane_selected_id(s->base.pane);
    (void)s;
    return selected && str_eq(selected, id);
}

static void sessions_layout(Screen *base, Doc *doc) {
    SessionsScreen *s = (SessionsScreen *)base;
    int w = doc->width;
    doc_space(doc, px(8));
    // The search field sits in a box the control fills.
    int box = doc_box_begin(doc, 0, w, px(6), theme.elevated, theme.border, px(10));
    doc_item(doc, box)->hover_fill = false;
    RECT sr = { px(30), doc->y, w - px(10), doc->y + px(22) };
    s->search_rc = sr;
    RECT gr = { px(8), doc->y, px(28), doc->y + px(22) };
    int gi = doc_text_at(doc, &gr, "", FONT_ICON_SMALL, theme.secondary, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    { wchar_t g[2] = { 0xE721, 0 }; char *u = wide_to_utf8(g); free(doc_item(doc, gi)->text); doc_item(doc, gi)->text = u; }
    doc->y += px(22);
    doc_box_end(doc, box, px(6));
    doc_space(doc, px(10));
    if (store_supports("pulls")) {
        char *id = xstrfmt("pulls:%s", s->project.repo);
        bool selected = is_selected(s, id);
        COLORREF fill = selected ? blend(theme.accent, theme.background, 0.16) : theme.elevated;
        int row = doc_box_begin(doc, 0, w, px(10), fill, selected ? blend(theme.accent, theme.background, 0.3) : theme.border, px(10));
        doc_label(doc, px(12), w - px(24), 0xE8AB, "Pull requests", FONT_BODY, theme.text);
        doc_box_end(doc, row, px(10));
        doc_box_action(doc, row, ACT_PULLS, 0);
        doc_space(doc, px(10));
        free(id);
    }
    if (s->error) { doc_notice(doc, px(4), w - px(8), s->error); doc_space(doc, px(8)); }
    size_t active = 0, shown = 0;
    for (size_t i = 0; i < s->count; i++) if (session_shown(s, &s->sessions[i])) { shown++; if (session_is_active(&s->sessions[i])) active++; }
    if (active) {
        doc_section(doc, 0, w, "Active");
        for (size_t i = 0; i < s->count; i++) {
            if (!session_shown(s, &s->sessions[i]) || !session_is_active(&s->sessions[i])) continue;
            char *id = xstrfmt("conversation:%s", session_id(&s->sessions[i]));
            doc_session_row(doc, 0, w, &s->sessions[i], ACT_OPEN_SESSION, (intptr_t)i, is_selected(s, id), theme.elevated);
            free(id);
            doc_space(doc, px(6));
        }
    }
    // Section header with the closed toggle on the right.
    doc_space(doc, px(14));
    int y = doc->y;
    const char *toggle = s->show_closed ? "Hide closed" : "Show closed";
    int tw = text_width(doc->hdc, toggle, FONT_CAPTION_MEDIUM) + px(16);
    RECT hr = { px(4), y, w - tw - px(8), y + font_height(doc->hdc, FONT_CAPTION_SEMIBOLD) + px(4) };
    doc_text_at(doc, &hr, active ? "Recent" : "Conversations", FONT_CAPTION_SEMIBOLD, theme.secondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT tr = { w - tw, y - px(2), w, hr.bottom + px(2) };
    int ti = doc_text_at(doc, &tr, toggle, FONT_CAPTION_MEDIUM, theme.accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    doc_item(doc, ti)->action = ACT_TOGGLE_CLOSED; doc_item(doc, ti)->hand = true;
    doc->y = hr.bottom + px(6);
    for (size_t i = 0; i < s->count; i++) {
        if (!session_shown(s, &s->sessions[i]) || session_is_active(&s->sessions[i])) continue;
        char *id = xstrfmt("conversation:%s", session_id(&s->sessions[i]));
        doc_session_row(doc, 0, w, &s->sessions[i], ACT_OPEN_SESSION, (intptr_t)i, is_selected(s, id), theme.elevated);
        free(id);
        doc_space(doc, px(6));
    }
    if (s->loaded && !shown) { doc_space(doc, px(8)); doc_text(doc, px(8), w - px(16), str_empty(s->query) ? "No conversations here yet." : "No matching conversations.", FONT_CALLOUT, theme.secondary, DT_LEFT | DT_WORDBREAK); }
    if (!s->loaded) doc_loading(doc, 0, w, "Loading\xE2\x80\xA6");
    doc_space(doc, px(12));
}
static void sessions_place(Screen *base, const RECT *content, int scroll_y) {
    SessionsScreen *s = (SessionsScreen *)base;
    int m = px(12);
    MoveWindow(s->search, content->left + m + s->search_rc.left, content->top + s->search_rc.top - scroll_y, s->search_rc.right - s->search_rc.left, s->search_rc.bottom - s->search_rc.top, TRUE);
}
static void sessions_header(Screen *base, HeaderInfo *info) {
    SessionsScreen *s = (SessionsScreen *)base;
    snprintf(info->title, sizeof info->title, "%s", project_title(&s->project));
    if (!str_eq(project_title(&s->project), s->project.repo)) snprintf(info->subtitle, sizeof info->subtitle, "%s", s->project.repo);
    if (store_supports("start_session")) {
        info->buttons[0].glyph = 0xE70F; info->buttons[0].action = ACT_NEW; info->buttons[0].enabled = !s->creating; info->buttons[0].tip = "New conversation";
        info->button_count = 1;
    }
}
static void sessions_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    SessionsScreen *s = (SessionsScreen *)base;
    switch (action) {
    case ACT_PULLS: app_show_detail(pulls_screen_new(&s->project)); break;
    case ACT_OPEN_SESSION: if ((size_t)arg < s->count) app_show_detail(conversation_screen_new(&s->sessions[arg])); break;
    case ACT_TOGGLE_CLOSED: s->show_closed = !s->show_closed; pane_relayout(base->pane); break;
    case ACT_NEW: {
        if (s->creating) break;
        s->creating = true; pane_header_changed(base->pane);
        Session started;
        bool ok = dialog_new_conversation(app_window(), &s->project, &started);
        s->creating = false; pane_header_changed(base->pane);
        if (ok) { app_show_detail(conversation_screen_new(&started)); session_free(&started); }
        // The list may already know the new conversation.
        request_cancel(&s->req); sessions_load(s);
        break;
    }
    }
}
static void sessions_timer(Screen *base, UINT id) {
    SessionsScreen *s = (SessionsScreen *)base;
    if (poller_fired(&s->poller, id)) { if (s->creating) poller_finished(&s->poller, false, -1); else sessions_load(s); }
}
static void sessions_visible(Screen *base, bool shown) {
    SessionsScreen *s = (SessionsScreen *)base;
    ShowWindow(s->search, shown ? SW_SHOW : SW_HIDE);
    if (shown) poller_start(&s->poller, base->pane, TIMER_POLL, 7000); else { poller_stop(&s->poller); request_cancel(&s->req); }
}
static void sessions_command(Screen *base, int id, int code, HWND control) {
    SessionsScreen *s = (SessionsScreen *)base;
    if (id == ID_SEARCH && code == EN_CHANGE) {
        int n = GetWindowTextLengthW(control);
        wchar_t *w = xmalloc(((size_t)n + 1) * sizeof *w); GetWindowTextW(control, w, n + 1);
        char *q = wide_to_utf8(w); free(w);
        set_string(&s->query, q); free(q);
        pane_relayout(base->pane);
    }
}
static void sessions_refresh(Screen *base) { SessionsScreen *s = (SessionsScreen *)base; request_cancel(&s->req); sessions_load(s); }
static void sessions_activated(Screen *base, bool active) { if (active) { SessionsScreen *s = (SessionsScreen *)base; poller_start(&s->poller, base->pane, TIMER_POLL, 7000); } }

static const ScreenVTable sessions_vt = {
    .destroy = sessions_destroy, .layout = sessions_layout, .header = sessions_header, .action = sessions_action,
    .timer = sessions_timer, .place = sessions_place, .visible = sessions_visible, .command = sessions_command,
    .refresh = sessions_refresh, .activated = sessions_activated,
};
Screen *sessions_screen_new(const Project *project) {
    SessionsScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &sessions_vt; s->base.id = xstrfmt("sessions:%s", project->repo);
    project_copy(&s->project, project);
    HWND parent = pane_hwnd(app_sidebar_pane());
    s->search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)ID_SEARCH, GetModuleHandleW(NULL), NULL);
    SendMessageW(s->search, WM_SETFONT, (WPARAM)font(FONT_CALLOUT), TRUE);
    SendMessageW(s->search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Find a conversation");
    theme_apply_control(s->search);
    return &s->base;
}
