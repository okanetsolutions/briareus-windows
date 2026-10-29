#include "screen_new_session_internal.h"

// MARK: - Layout

typedef struct { char *before, *name, *after; } WelcomeData;

static void loop_done(void *owner, Request *req);
static void start_done(void *owner, Request *req);
static void welcome_free(void *p);
static void paint_welcome_line(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void new_session_layout(Screen *base, Doc *doc);
static void new_session_header(Screen *base, HeaderInfo *info);
static void new_session_timer(Screen *base, UINT id);
static void projects_done(void *owner, Request *req);
static void new_session_visible(Screen *base, bool shown);
static void new_session_activated(Screen *base, bool active);
static void new_session_destroy(Screen *base);
static void new_session_adopt(Screen *base, Screen *fresh_);

const Project *screen_new_session_project(NewSessionScreen *s) { return s->count ? &s->projects[s->chosen < s->count ? s->chosen : 0] : NULL; }

/// The branch chip's "no pick" row: a worktree branches off the default, the local checkout stays on what it has out.
char *screen_new_session_no_branch_label(NewSessionScreen *s) { return s->workspace == WORKSPACE_LOCAL ? xstrdup("Current branch") : xstrfmt("New branch off %s", s->default_branch ? s->default_branch : "main"); }

// MARK: - Starting

static void loop_done(void *owner, Request *req) { (void)owner; (void)req; }

static void start_done(void *owner, Request *req) {
    NewSessionScreen *s = owner;
    s->busy = false;
    Session started;
    if (req->ok && session_parse(json_get(req->result, "session"), &started)) {
        screen_new_session_set_composer_text(s, "");
        attach_list_sent(s->files, json_get(req->args, "attachments"));
        // Only worktree sessions have a review-loop chip; local sessions and orchestrators never have their own loop.
        if (s->workspace == WORKSPACE_WORKTREE && !s->review_loop && store_supports("review_loop") && session_can_review_loop(&started)) {
            Json *a = json_object(); json_set_str(a, "sessionId", session_id(&started)); json_set_bool(a, "on", false);
            store_call("review_loop", a, 0, NULL, loop_done, 0, NULL);
        }
        app_show_detail(conversation_screen_new(&started));
        session_free(&started);
        return;
    }
    char *text = request_error_or_unexpected(req);
    set_string(&s->error, text); free(text);
    if (request_outcome_unknown(req)) s->uncertain = true;
    pane_footer_changed(s->base.pane);
}

void screen_new_session_start(NewSessionScreen *s) {
    const Project *p = screen_new_session_project(s);
    if (!screen_new_session_can_start(s) || !store_supports("start_session")) return;
    char *prompt = screen_new_session_composer_text(s);
    Json *args = json_object();
    json_set_str(args, "repo", p->repo);
    if (!screen_new_session_composer_empty(s)) json_set_str(args, "prompt", prompt);
    Json *ids = attach_list_ids(s->files);
    if (ids) json_object_set(args, "attachments", ids);
    if (s->workspace == WORKSPACE_ORCHESTRATOR) json_set_bool(args, "orchestrator", true);
    else {
        if (s->branch) json_set_str(args, "branch", s->branch);
        if (s->workspace == WORKSPACE_LOCAL) json_set_bool(args, "local", true);
    }
    free(prompt);
    if (s->has_runtime) { Json *rt = runtime_choice_arguments(&s->runtime); json_object_merge(args, rt); json_free(rt); }
    s->busy = true; set_string(&s->error, NULL);
    store_call("start_session", args, 0, s, start_done, 0, &s->req_start);
    pane_footer_changed(s->base.pane);
}

static void welcome_free(void *p) { WelcomeData *d = p; free(d->before); free(d->name); free(d->after); free(d); }

static void paint_welcome_line(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    WelcomeData *d = it->data;
    int bw = text_width(cv, d->before, FONT_BODY), nw = text_width(cv, d->name, FONT_BODY), aw = text_width(cv, d->after, FONT_BODY);
    int w = rc->right - rc->left, lh = px(24);
    if (bw + nw + aw <= w) {
        int x = rc->left + (w - bw - nw - aw) / 2;
        RECT b = { x, rc->top, x + bw, rc->top + lh }; draw_text(cv, d->before, &b, FONT_BODY, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT n = { x + bw, rc->top, x + bw + nw, rc->top + lh }; draw_text(cv, d->name, &n, FONT_BODY, theme.accent, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT a = { x + bw + nw, rc->top, x + bw + nw + aw, rc->top + lh }; draw_text(cv, d->after, &a, FONT_BODY, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        int x = rc->left + (w - bw - nw) / 2; if (x < rc->left) x = rc->left;
        RECT b = { x, rc->top, x + bw, rc->top + lh }; draw_text(cv, d->before, &b, FONT_BODY, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT n = { x + bw, rc->top, x + bw + nw, rc->top + lh }; draw_text(cv, d->name, &n, FONT_BODY, theme.accent, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT a = { rc->left, rc->top + lh, rc->right, rc->top + lh * 2 }; draw_text(cv, d->after, &a, FONT_BODY, theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static void new_session_layout(Screen *base, Doc *doc) {
    NewSessionScreen *s = (NewSessionScreen *)base;
    int w = doc->width;
    RECT rc = pane_content_rect(base->pane);
    // `#welcome`: `mx-auto mt-[14vh] max-w-[640px] text-center`.
    int col = w < px(640) ? w : px(640), x = (w - col) / 2;
    doc_space(doc, (rc.bottom - rc.top) * 14 / 100);
    doc_text(doc, x, col, "Welcome back", FONT_LARGE_TITLE, theme.ink, DT_CENTER | DT_SINGLELINE);
    doc_space(doc, px(6));
    const Project *p = screen_new_session_project(s);
    WelcomeData *d = xcalloc(1, sizeof *d);
    if (s->workspace == WORKSPACE_ORCHESTRATOR) { d->before = xstrdup("Start an orchestrator for "); d->name = xstrdup(p ? project_title(p) : "the project"); d->after = xstrdup(" to plan and coordinate worker sessions."); }
    else if (s->workspace == WORKSPACE_LOCAL) { d->before = xstrdup("Start a session in "); d->name = xstrdup(p ? project_title(p) : "the project"); d->after = xstrdup("'s own local checkout and database."); }
    else { d->before = xstrdup("Start a session in a fresh "); d->name = xstrdup(p ? project_title(p) : "project"); d->after = xstrdup(" checkout with its own database."); }
    int bw = text_width(doc->cv, d->before, FONT_BODY) + text_width(doc->cv, d->name, FONT_BODY) + text_width(doc->cv, d->after, FONT_BODY);
    doc_custom(doc, x, col, bw <= col ? px(24) : px(48), paint_welcome_line, d, welcome_free, 0, 0);
    doc_text(doc, x, col, "Pick a provider and model below, then describe what to build.", FONT_BODY, theme.muted, DT_CENTER | DT_WORDBREAK);
    if (!s->count) { doc_space(doc, px(16)); doc_text(doc, x, col, "No projects yet, so there is nothing to build. Add one in Settings \xE2\x86\x92 Projects on the web dashboard.", FONT_FOOTNOTE, theme.muted, DT_CENTER | DT_WORDBREAK); }
}

static void new_session_header(Screen *base, HeaderInfo *info) { (void)base; (void)info; }

// MARK: - Lifecycle

static void new_session_timer(Screen *base, UINT id) {
    NewSessionScreen *s = (NewSessionScreen *)base;
    if (id == TIMER_VOICE) { if (s->voice) voice_tick(s->voice); pane_footer_changed(base->pane); }
}

static void projects_done(void *owner, Request *req) {
    NewSessionScreen *s = owner;
    Project *items; size_t n;
    if (!req->ok || !projects_parse(req->result, &items, &n)) return;
    // The project already picked stays picked.
    const Project *p = screen_new_session_project(s);
    char *keep = xstrdup(p ? p->repo : s->asked ? s->asked : "");
    size_t chosen = 0;
    for (size_t i = 0; i < n; i++) if (str_eq(items[i].repo, keep)) chosen = i;
    free(keep);
    projects_free(s->projects, s->count);
    s->projects = items; s->count = n; s->chosen = chosen;
    screen_new_session_load_choices(s);
    pane_relayout(s->base.pane); pane_footer_changed(s->base.pane);
}

static void new_session_visible(Screen *base, bool shown) {
    NewSessionScreen *s = (NewSessionScreen *)base;
    if (shown) {
        ShowWindow(s->composer, SW_SHOW);
        if (!s->count && !s->req_projects) store_call("projects", json_object(), 0, s, projects_done, 0, &s->req_projects);
        if (s->count && !s->has_catalog && !s->req_runtimes) screen_new_session_load_choices(s);
    }
    else { ShowWindow(s->composer, SW_HIDE); KillTimer(pane_hwnd(base->pane), TIMER_VOICE); if (s->voice) voice_drop(s->voice); }
}

static void new_session_activated(Screen *base, bool active) {
    NewSessionScreen *s = (NewSessionScreen *)base;
    if (!active && s->voice && voice_state(s->voice) == VOICE_RECORDING) voice_stop(s->voice);
}

static void new_session_destroy(Screen *base) {
    NewSessionScreen *s = (NewSessionScreen *)base;
    request_cancel(&s->req_runtimes); request_cancel(&s->req_branches); request_cancel(&s->req_start); request_cancel(&s->req_projects);
    if (base->pane) KillTimer(pane_hwnd(base->pane), TIMER_VOICE);
    if (s->voice) voice_free(s->voice);
    attach_list_free(s->files);
    if (s->composer) DestroyWindow(s->composer);
    projects_free(s->projects, s->count);
    if (s->has_catalog) runtime_catalog_free(&s->catalog);
    if (s->has_runtime) runtime_choice_free(&s->runtime);
    str_array_free(s->branches, s->branch_count); free(s->default_branch); free(s->branch); free(s->error); free(s->asked);
    screen_release(base);
}

/// ＋ New session again while this one shows: it moves to the project the sidebar has open, the prompt kept.
static void new_session_adopt(Screen *base, Screen *fresh_) {
    NewSessionScreen *s = (NewSessionScreen *)base, *fresh = (NewSessionScreen *)fresh_;
    if (!fresh->asked || s->busy) return;
    set_string(&s->asked, fresh->asked);
    const Project *p = screen_new_session_project(s);
    if (p && str_eq(p->repo, fresh->asked)) return;
    size_t i = 0;
    while (i < s->count && !str_eq(s->projects[i].repo, fresh->asked)) i++;
    if (i == s->count) {
        const Project *wanted = screen_new_session_project(fresh);
        if (!wanted || !str_eq(wanted->repo, fresh->asked)) return;
        s->projects = xrealloc(s->projects, (s->count + 1) * sizeof *s->projects);
        project_copy(&s->projects[s->count++], wanted);
    }
    s->chosen = i;
    set_string(&s->error, NULL); s->uncertain = false;
    screen_new_session_load_choices(s);
    pane_relayout(base->pane); pane_footer_changed(base->pane);
}

static const ScreenVTable new_session_vt = {
    .destroy = new_session_destroy, .layout = new_session_layout, .header = new_session_header,
    .footer_height = screen_new_session_new_session_footer_height, .footer_layout = screen_new_session_new_session_footer_layout, .footer_paint = screen_new_session_new_session_footer_paint,
    .footer_click = screen_new_session_new_session_footer_click, .visible = new_session_visible, .command = screen_new_session_new_session_command, .timer = new_session_timer,
    .activated = new_session_activated, .adopt = new_session_adopt,
};

Screen *new_session_screen_new(const Project *project_, const Project *projects, size_t count) {
    NewSessionScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &new_session_vt; s->base.id = xstrdup("new-session");
    s->workspace = WORKSPACE_WORKTREE;
    if (project_) s->asked = xstrdup(project_->repo);
    s->projects = xcalloc(count, sizeof *s->projects);
    for (size_t i = 0; i < count; i++) { project_copy(&s->projects[i], &projects[i]); if (project_ && str_eq(projects[i].repo, project_->repo)) s->chosen = i; }
    s->count = count;
    if (project_ && !count) { s->projects = xrealloc(s->projects, sizeof *s->projects); project_copy(&s->projects[0], project_); s->count = 1; }
    if (!s->count) {
        // Opened before the sidebar has its list: what was saved serves until the server answers.
        Json *saved = cache_value(g_store.cache, "projects");
        Project *items; size_t n;
        if (saved && projects_parse(saved, &items, &n)) { free(s->projects); s->projects = items; s->count = n; }
        json_free(saved);
    }
    s->review_loop = settings_read_review_loop();
    s->composer_lines = 1;
    HWND parent = pane_hwnd(app_detail_pane());
    s->composer = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)ID_COMPOSER, GetModuleHandleW(NULL), NULL);
    SendMessageW(s->composer, WM_SETFONT, (WPARAM)font(FONT_BODY), TRUE);
    SendMessageW(s->composer, EM_SETCUEBANNER, TRUE, (LPARAM)L"Describe what to build\x2026");
    SetWindowSubclass(s->composer, screen_new_session_composer_proc, ID_COMPOSER, (DWORD_PTR)s);
    theme_apply_control(s->composer);
    DragAcceptFiles(s->composer, TRUE);
    s->files = attach_list_new(&s->base, "start_session");
    const Project *picked = screen_new_session_project(s);
    attach_list_scope(s->files, picked ? picked->repo : NULL);
    if (store_can_transcribe()) s->voice = voice_new(screen_new_session_voice_changed, s);
    return &s->base;
}
