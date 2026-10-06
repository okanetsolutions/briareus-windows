// The dashboard's opening view: "Welcome back", and the composer that starts a session, with its row of chips for the
// project, branch, provider, model, effort and the loops.
#include "attach_list.h"
#include "credentials.h"
#include "screens.h"
#include "str.h"
#include "voice.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ID_COMPOSER = 401 };
enum { TIMER_VOICE = 3 };
enum { CHIP_WORKSPACE, CHIP_PROJECT, CHIP_BRANCH, CHIP_PROVIDER, CHIP_MODEL, CHIP_EFFORT, CHIP_LOOP, CHIP_COUNT };

typedef struct {
    Screen base;
    Project *projects; size_t count; size_t chosen;
    char *asked;   // the repo of the project the screen was opened for; NULL: none in particular
    bool has_catalog; RuntimeCatalog catalog;
    bool has_runtime; RuntimeChoice runtime;   // the pick; none starts on the project default
    bool hand_picked;   // the pick came from the chips rather than from the last pick or the first available provider
    char **branches; size_t branch_count; char *default_branch; char *branch;   // NULL: a new branch off the default
    bool local;   // work in the project's own checkout rather than a fresh worktree
    bool review_loop;
    Request *req_runtimes, *req_branches, *req_start, *req_projects;
    bool busy, uncertain;
    char *error;
    HWND composer; int composer_lines; RECT composer_rc; bool focused;
    VoiceNote *voice;
    AttachList *files;
    // footer hit rects, from the last paint
    RECT chip_rc[CHIP_COUNT]; bool chip_on[CHIP_COUNT];
    RECT mic_rc, send_rc, box_rc, attach_rc;
    int chips_h;
} NewSessionScreen;

static const Project *project(NewSessionScreen *s) { return s->count ? &s->projects[s->chosen < s->count ? s->chosen : 0] : NULL; }
/// The branch chip's "no pick" row: a worktree branches off the default, the local checkout stays on what it has out.
static char *no_branch_label(NewSessionScreen *s) { return s->local ? xstrdup("Current branch") : xstrfmt("New branch off %s", s->default_branch ? s->default_branch : "main"); }

// MARK: - The composer's text

static char *composer_text(NewSessionScreen *s) {
    int n = GetWindowTextLengthW(s->composer);
    wchar_t *w = xmalloc(((size_t)n + 1) * sizeof *w); GetWindowTextW(s->composer, w, n + 1);
    char *text = wide_to_utf8(w); free(w);
    char *lf = str_replace(text, "\r\n", "\n"); free(text);
    return lf;
}
static void set_composer_text(NewSessionScreen *s, const char *text) {
    char *crlf = str_replace(text ? text : "", "\n", "\r\n");
    wchar_t *w = utf8_to_wide(crlf);
    SetWindowTextW(s->composer, w);
    SendMessageW(s->composer, EM_SETSEL, (WPARAM)wcslen(w), (LPARAM)wcslen(w));
    free(w); free(crlf);
    pane_footer_changed(s->base.pane);
}
static bool composer_empty(NewSessionScreen *s) { char *t = composer_text(s); char *trimmed = str_trim(t); bool e = !*trimmed; free(t); free(trimmed); return e; }
/// A session starts on a prompt, on files, or on both; never while a file is still uploading.
static bool can_start(NewSessionScreen *s) {
    return !s->busy && !s->uncertain && project(s) && (!composer_empty(s) || attach_list_count(s->files)) && !attach_list_uploading(s->files);
}

// MARK: - What the chips offer

static bool effective_choice(NewSessionScreen *s, RuntimeChoice *out) {
    if (s->has_runtime) { runtime_choice_copy(out, &s->runtime); return true; }
    if (s->has_catalog && s->catalog.has_default) { runtime_choice_copy(out, &s->catalog.def); return true; }
    return false;
}
/// Takes a project's catalog: a hand pick or else the last pick where this project still offers it; without a project
/// default a start needs a provider. The cached catalog comes first, so each catalog decides the screen's own picks afresh.
static void adopt(NewSessionScreen *s, RuntimeCatalog c) {
    if (s->has_catalog) runtime_catalog_free(&s->catalog);
    s->catalog = c; s->has_catalog = true;
    if (s->has_runtime) {
        RuntimeChoice kept;
        bool keep = s->hand_picked && runtime_catalog_offered(&s->catalog, &s->runtime, &kept);
        runtime_choice_free(&s->runtime); s->has_runtime = false;
        if (keep) { s->runtime = kept; s->has_runtime = true; return; }
        s->hand_picked = false;
    }
    char *text = settings_read_last_runtime(); RuntimeChoice last;
    if (runtime_choice_from_saved(text, &last)) { s->has_runtime = runtime_catalog_offered(&s->catalog, &last, &s->runtime); runtime_choice_free(&last); }
    free(text);
    if (!s->has_runtime && !s->catalog.has_default) s->has_runtime = runtime_catalog_first_available(&s->catalog, &s->runtime);
}
/// A pick made by hand, kept for the next new session; the screen's own picks (the first available provider) are not.
static void remember(NewSessionScreen *s) {
    s->hand_picked = true;
    char *text = runtime_choice_saved(&s->runtime); settings_write_last_runtime(text); free(text);
}
static void runtimes_done(void *owner, Request *req) {
    NewSessionScreen *s = owner;
    if (!req->ok || !project(s)) return;
    RuntimeCatalog c;
    if (!runtime_catalog_parse(req->result, &c)) return;
    adopt(s, c);
    char *key = xstrfmt("runtimes:%s", project(s)->repo); cache_store(g_store.cache, req->result, key); free(key);
    pane_footer_changed(s->base.pane);
}
static void branches_done(void *owner, Request *req) {
    NewSessionScreen *s = owner;
    if (!req->ok || !project(s)) return;
    str_array_free(s->branches, s->branch_count); s->branches = NULL; s->branch_count = 0;
    const Json *list = json_get(req->result, "branches");
    s->branches = xmalloc((json_count(list) ? json_count(list) : 1) * sizeof *s->branches);
    for (size_t i = 0; i < json_count(list); i++) { const char *b = json_str(json_at(list, i)); if (b) s->branches[s->branch_count++] = xstrdup(b); }
    set_string(&s->default_branch, json_str(json_get(req->result, "defaultBranch")));
    char *key = xstrfmt("branches:%s", project(s)->repo); cache_store(g_store.cache, req->result, key); free(key);
    pane_footer_changed(s->base.pane);
}
/// Reads what the picked project offers: its runtimes and branches, the saved answer first.
static void load_choices(NewSessionScreen *s) {
    const Project *p = project(s);
    request_cancel(&s->req_runtimes); request_cancel(&s->req_branches);
    if (s->has_catalog) { runtime_catalog_free(&s->catalog); s->has_catalog = false; }
    if (s->has_runtime) { runtime_choice_free(&s->runtime); s->has_runtime = false; }
    s->hand_picked = false;
    str_array_free(s->branches, s->branch_count); s->branches = NULL; s->branch_count = 0;
    set_string(&s->default_branch, NULL); set_string(&s->branch, NULL);
    if (!p || !p->has_local) s->local = false;
    if (!p) return;
    if (store_supports("runtimes")) {
        char *key = xstrfmt("runtimes:%s", p->repo);
        Json *saved = cache_value(g_store.cache, key);
        if (saved) { RuntimeCatalog c; if (runtime_catalog_parse(saved, &c)) adopt(s, c); json_free(saved); }
        free(key);
        Json *args = json_object(); json_set_str(args, "repo", p->repo);
        store_call("runtimes", args, 0, s, runtimes_done, 0, &s->req_runtimes);
    }
    if (store_supports("branches")) {
        char *key = xstrfmt("branches:%s", p->repo);
        Json *saved = cache_value(g_store.cache, key);
        if (saved) {
            const Json *list = json_get(saved, "branches");
            s->branches = xmalloc((json_count(list) ? json_count(list) : 1) * sizeof *s->branches);
            for (size_t i = 0; i < json_count(list); i++) { const char *b = json_str(json_at(list, i)); if (b) s->branches[s->branch_count++] = xstrdup(b); }
            set_string(&s->default_branch, json_str(json_get(saved, "defaultBranch")));
            json_free(saved);
        }
        free(key);
        Json *args = json_object(); json_set_str(args, "repo", p->repo);
        store_call("branches", args, 0, s, branches_done, 0, &s->req_branches);
    }
}

// MARK: - Starting

static void loop_done(void *owner, Request *req) { (void)owner; (void)req; }
static void start_done(void *owner, Request *req) {
    NewSessionScreen *s = owner;
    s->busy = false;
    Session started;
    if (req->ok && session_parse(json_get(req->result, "session"), &started)) {
        set_composer_text(s, "");
        attach_list_sent(s->files, json_get(req->args, "attachments"));
        // The loop the chip asked for that the server does not arm by default. A local session never has one.
        if (!s->local && !s->review_loop && store_supports("review_loop") && session_can_review_loop(&started)) {
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
static void start(NewSessionScreen *s) {
    const Project *p = project(s);
    if (!can_start(s) || !store_supports("start_session")) return;
    char *prompt = composer_text(s);
    Json *args = json_object();
    json_set_str(args, "repo", p->repo);
    if (!composer_empty(s)) json_set_str(args, "prompt", prompt);
    Json *ids = attach_list_ids(s->files);
    if (ids) json_object_set(args, "attachments", ids);
    if (s->branch) json_set_str(args, "branch", s->branch);
    if (s->local) json_set_bool(args, "local", true);
    free(prompt);
    if (s->has_runtime) { Json *rt = runtime_choice_arguments(&s->runtime); json_object_merge(args, rt); json_free(rt); }
    s->busy = true; set_string(&s->error, NULL);
    store_call("start_session", args, 0, s, start_done, 0, &s->req_start);
    pane_footer_changed(s->base.pane);
}

static LRESULT CALLBACK composer_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    NewSessionScreen *s = (NewSessionScreen *)ref;
    if (msg == WM_KEYDOWN && wp == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_CONTROL) & 0x8000)) { start(s); return 0; }
    if (msg == WM_CHAR && wp == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_CONTROL) & 0x8000)) return 0;
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { SetFocus(GetParent(hwnd)); return 0; }
    // Ctrl+V and Shift+Insert both reach the control as WM_PASTE; a file or image becomes an attachment, text pastes as ever.
    if (msg == WM_PASTE && attach_list_paste(s->files)) return 0;
    if (msg == WM_DROPFILES) { attach_list_drop(s->files, (HDROP)wp); return 0; }
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) { s->focused = msg == WM_SETFOCUS; pane_footer_changed(s->base.pane); }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, composer_proc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// MARK: - Voice

static void voice_changed(void *ctx) {
    NewSessionScreen *s = ctx;
    char *text = voice_take_text(s->voice);
    if (text) {
        if (*text) {
            char *current = composer_text(s);
            size_t n = strlen(current);
            const char *gap = (!n || current[n - 1] == ' ' || current[n - 1] == '\n') ? "" : " ";
            char *joined = xstrfmt("%s%s%s", current, gap, text);
            set_composer_text(s, joined);
            free(joined); free(current);
        }
        free(text);
    }
    char *error = voice_take_error(s->voice);
    if (error) { app_alert("Voice note", error); free(error); }
    if (voice_state(s->voice) == VOICE_RECORDING) SetTimer(pane_hwnd(s->base.pane), TIMER_VOICE, 1000, NULL); else KillTimer(pane_hwnd(s->base.pane), TIMER_VOICE);
    pane_footer_changed(s->base.pane);
}

// MARK: - Layout

typedef struct { char *before, *name, *after; } WelcomeData;
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
    const Project *p = project(s);
    WelcomeData *d = xcalloc(1, sizeof *d);
    if (s->local) { d->before = xstrdup("Start a session in "); d->name = xstrdup(p ? project_title(p) : "the project"); d->after = xstrdup("'s own local checkout and database."); }
    else { d->before = xstrdup("Start a session in a fresh "); d->name = xstrdup(p ? project_title(p) : "project"); d->after = xstrdup(" checkout with its own database."); }
    int bw = text_width(doc->cv, d->before, FONT_BODY) + text_width(doc->cv, d->name, FONT_BODY) + text_width(doc->cv, d->after, FONT_BODY);
    doc_custom(doc, x, col, bw <= col ? px(24) : px(48), paint_welcome_line, d, welcome_free, 0, 0);
    doc_text(doc, x, col, "Pick a provider and model below, then describe what to build.", FONT_BODY, theme.muted, DT_CENTER | DT_WORDBREAK);
    if (!s->count) { doc_space(doc, px(16)); doc_text(doc, x, col, "No projects yet, so there is nothing to build. Add one in Settings \xE2\x86\x92 Projects on the web dashboard.", FONT_FOOTNOTE, theme.muted, DT_CENTER | DT_WORDBREAK); }
}
static void new_session_header(Screen *base, HeaderInfo *info) { (void)base; (void)info; }

// MARK: - Footer: the chips and the composer

static char *chip_label(NewSessionScreen *s, int chip) {
    const Project *p = project(s);
    RuntimeChoice eff; bool has = effective_choice(s, &eff);
    char *out = NULL;
    switch (chip) {
    case CHIP_WORKSPACE: out = xstrdup(s->local ? "\xE2\x8C\x82 Local" : "\xE2\x8C\x97 Worktree"); break;
    case CHIP_PROJECT: out = xstrdup(p ? project_title(p) : "Project"); break;
    case CHIP_BRANCH: out = s->branch ? xstrdup(s->branch) : no_branch_label(s); break;
    case CHIP_PROVIDER: {
        const RuntimeProvider *pr = has ? runtime_catalog_provider(&s->catalog, eff.provider_id) : NULL;
        out = xstrdup(pr ? pr->label : "Provider");
        break;
    }
    case CHIP_MODEL: {
        const RuntimeModel *m = has ? runtime_catalog_model(&s->catalog, &eff) : NULL;
        out = xstrdup(m ? runtime_model_title(m) : has && eff.model ? eff.model : "Model");
        break;
    }
    case CHIP_EFFORT: out = xstrdup(has && eff.effort ? eff.effort : "effort"); break;
    case CHIP_LOOP: out = xstrdup(s->review_loop ? "\xF0\x9F\x94\x81 Review loop: on" : "\xF0\x9F\x94\x81 Review loop"); break;
    }
    if (has) runtime_choice_free(&eff);
    return out;
}
static bool chip_shown(NewSessionScreen *s, int chip) {
    switch (chip) {
    case CHIP_PROVIDER: case CHIP_MODEL: return s->has_catalog && s->catalog.provider_count > 0;
    case CHIP_EFFORT: { RuntimeChoice eff; if (!effective_choice(s, &eff)) return false; size_t n = 0; runtime_catalog_efforts(&s->catalog, &eff, &n); runtime_choice_free(&eff); return n > 0; }
    case CHIP_BRANCH: return store_supports("branches");
    case CHIP_LOOP: return !s->local && store_supports("review_loop");   // the server arms no loop on the shared checkout
    default: return true;
    }
}
static bool chip_picker(int chip) { return chip != CHIP_LOOP; }
static int chip_width(Canvas *cv, const char *label, bool picker) {
    int w = px(6) * 2 + text_width(cv, label, FONT_CAPTION) + px(4);   // a glyph from a fallback font measures a touch narrow
    if (picker) w += px(4) + text_width(cv, "\xE2\x96\xBE", FONT_TINY_SEMIBOLD);
    if (w > px(150)) w = px(150);
    return w;
}
/// Places the chips in rows 4px apart, wrapping at `width`; returns the rows' height. NULL `out` measures only.
static int chips_layout(NewSessionScreen *s, Canvas *cv, int width, RECT *out) {
    int x = 0, y = 0, h = px(24), gap = px(4);
    for (int c = 0; c < CHIP_COUNT; c++) {
        if (out) SetRectEmpty(&out[c]);
        if (!chip_shown(s, c)) continue;
        char *label = chip_label(s, c);
        int w = chip_width(cv, label, chip_picker(c));
        free(label);
        if (x > 0 && x + w > width) { x = 0; y += h + gap; }
        if (out) { RECT r = { x, y, x + w, y + h }; out[c] = r; }
        x += w + gap;
    }
    return y + h;
}
static int composer_height(NewSessionScreen *s, Canvas *cv) {
    int line_h = px(24);
    int lines = s->composer_lines < 1 ? 1 : s->composer_lines > 8 ? 8 : s->composer_lines;
    (void)cv;
    return lines * line_h;
}
/// `#composer-wrap`: 8px above, the chips, 8px, the box (10px, the files, the text, 6px, the 30px buttons, 8px), 6px, a
/// 16px note, 14px below.
static int new_session_footer_height(Screen *base, int width) {
    NewSessionScreen *s = (NewSessionScreen *)base;
    Canvas *cv = NULL;   // measuring only
    int inner = width - px(24) * 2; if (inner > px(860)) inner = px(860);
    int chips = chips_layout(s, cv, inner, NULL);
    int files = attach_list_height(s->files, cv, inner - px(24));
    int h = px(8) + chips + px(8) + (px(10) + files + composer_height(s, cv) + px(6) + px(30) + px(8) + 2) + px(6) + px(16) + px(14);
    return h;
}
static RECT footer_column(const RECT *rc) {
    int w = rc->right - rc->left - px(24) * 2; if (w > px(860)) w = px(860);
    int x = rc->left + (rc->right - rc->left - w) / 2;
    RECT r = { x, rc->top, x + w, rc->bottom };
    return r;
}
static void new_session_footer_layout(Screen *base, const RECT *rc) {
    NewSessionScreen *s = (NewSessionScreen *)base;
    Canvas *cv = NULL;   // measuring only
    RECT col = footer_column(rc);
    int chips = chips_layout(s, cv, col.right - col.left, NULL);
    int files = attach_list_height(s->files, cv, col.right - col.left - px(24));
    int ch = composer_height(s, cv);
    int top = col.top + px(8) + chips + px(8);
    RECT er = { col.left + px(12) + 1, top + px(10) + 1 + files, col.right - px(12) - 1, top + px(10) + 1 + files + ch };
    s->composer_rc = er;
    pane_place_control(s->composer, &er);
}
static void new_session_footer_paint(Screen *base, Canvas *cv, const RECT *rc) {
    NewSessionScreen *s = (NewSessionScreen *)base;
    fill_rect(cv, rc, theme.canvas);
    RECT col = footer_column(rc);
    int width = col.right - col.left;
    // The chips.
    RECT rects[CHIP_COUNT];
    int chips = chips_layout(s, cv, width, rects);
    int y0 = col.top + px(8);
    for (int c = 0; c < CHIP_COUNT; c++) {
        SetRectEmpty(&s->chip_rc[c]);
        if (IsRectEmpty(&rects[c])) continue;
        RECT r = { col.left + rects[c].left, y0 + rects[c].top, col.left + rects[c].right, y0 + rects[c].bottom };
        s->chip_rc[c] = r;
        bool on = (c == CHIP_LOOP && s->review_loop);
        bool dim = s->busy;
        fill_round_rect(cv, &r, px(6), theme.raise, on ? theme.accent : theme.line);
        char *label = chip_label(s, c);
        bool picker = chip_picker(c);
        int cw = picker ? px(4) + text_width(cv, "\xE2\x96\xBE", FONT_TINY_SEMIBOLD) : 0;
        RECT t = { r.left + px(6), r.top, r.right - px(6) - cw, r.bottom };
        draw_text(cv, label, &t, FONT_CAPTION, on ? theme.accent : dim ? theme.muted : theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (picker) { RECT a = { r.right - px(6) - cw + px(4), r.top, r.right - px(6), r.bottom }; draw_text(cv, "\xE2\x96\xBE", &a, FONT_TINY_SEMIBOLD, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE); }
        free(label);
    }
    // The box: `rounded-2xl border border-line bg-raise px-3 pt-2.5 pb-2 focus-within:border-line-strong`.
    int top = y0 + chips + px(8);
    int ch = composer_height(s, cv);
    int files = attach_list_height(s->files, cv, width - px(24));
    RECT box = { col.left, top, col.right, top + px(10) + files + ch + px(6) + px(30) + px(8) + 2 };
    s->box_rc = box;
    fill_round_rect(cv, &box, px(16), theme.raise, s->focused ? theme.line_strong : theme.line);
    attach_list_paint(s->files, cv, box.left + px(12), box.top + px(10) + 1, width - px(24));
    int row_y = box.top + px(10) + 1 + files + ch + px(6), bh = px(30), bw = px(32);
    memset(&s->mic_rc, 0, sizeof s->mic_rc); memset(&s->attach_rc, 0, sizeof s->attach_rc);
    int x = box.left + px(12);
    // `#btn-attach`: the 📎 left of the microphone, for files the clipboard or a drop cannot bring.
    if (attach_list_supported(s->files)) {
        RECT attach = { x, row_y, x + bw, row_y + bh };
        s->attach_rc = attach;
        attach_button_paint(cv, &attach, !s->busy && attach_list_count(s->files) < ATTACHMENTS_MAX);
        x += bw + px(6);
    }
    VoiceState vs = s->voice ? voice_state(s->voice) : VOICE_IDLE;
    if (store_can_transcribe()) {
        RECT mic = { x, row_y, x + bw, row_y + bh };
        s->mic_rc = mic;
        fill_round_rect(cv, &mic, px(8), vs == VOICE_RECORDING ? theme.danger : theme.raise, vs == VOICE_RECORDING ? theme.danger : theme.line);
        draw_text(cv, vs == VOICE_RECORDING ? "\xE2\x96\xA0" : "\xF0\x9F\x8E\xA4", &mic, FONT_EMOJI_LARGE, vs == VOICE_RECORDING ? theme.white : theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (vs == VOICE_RECORDING) { char *clock = format_clock(voice_elapsed(s->voice)); RECT cr = { mic.right + px(8), row_y, mic.right + px(80), row_y + bh }; draw_text(cv, clock, &cr, FONT_CAPTION, theme.danger, DT_LEFT | DT_VCENTER | DT_SINGLELINE); free(clock); }
    }
    RECT send = { box.right - px(12) - bw, row_y, box.right - px(12), row_y + bh };
    s->send_rc = send;
    bool enabled = can_start(s);
    fill_round_rect(cv, &send, px(8), enabled ? theme.accent : theme.field, enabled ? theme.accent : theme.field);
    draw_text(cv, "\xE2\x86\xB5", &send, FONT_BODY, enabled ? theme.on_accent : theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    // The note under the box.
    RECT note = { col.left, box.bottom + px(6), col.right, box.bottom + px(6) + px(16) };
    const char *text = s->busy ? "Starting the session\xE2\x80\xA6" : s->error ? s->error : attach_list_uploading(s->files) ? "Uploading\xE2\x80\xA6" : "";
    draw_text(cv, text, &note, FONT_FOOTNOTE, s->error && !s->busy ? theme.danger : theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// MARK: - Menus

static int popup(NewSessionScreen *s, HMENU menu, const RECT *anchor) {
    POINT pt = { anchor->left, anchor->bottom + px(4) };
    ClientToScreen(pane_hwnd(s->base.pane), &pt);
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(s->base.pane), NULL);
    DestroyMenu(menu);
    return chosen;
}
static void append(HMENU menu, UINT id, const char *text, bool checked, bool enabled) {
    wchar_t *w = utf8_to_wide(text);
    AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED), id, w);
    free(w);
}
static void pick_chip(NewSessionScreen *s, int chip) {
    if (s->busy) return;
    const RECT *r = &s->chip_rc[chip];
    RuntimeChoice eff; bool has = effective_choice(s, &eff);
    switch (chip) {
    case CHIP_WORKSPACE: {
        const Project *p = project(s);
        bool can = p && p->has_local;
        HMENU m = CreatePopupMenu();
        append(m, 1, "\xE2\x8C\x97 Worktree: a fresh clone and database", !s->local, true);
        append(m, 2, can ? "\xE2\x8C\x82 Local: the project's own checkout" : "\xE2\x8C\x82 Local: no local checkout set in Settings", s->local, can);
        int chosen = popup(s, m, r);
        if (chosen >= 1 && (chosen == 2) != s->local) {
            // "No branch picked" means something else in each mode, so the pick starts over.
            s->local = chosen == 2; set_string(&s->branch, NULL);
            pane_relayout(s->base.pane); pane_footer_changed(s->base.pane);
        }
        break;
    }
    case CHIP_PROJECT: {
        HMENU m = CreatePopupMenu();
        for (size_t i = 0; i < s->count; i++) append(m, (UINT)i + 1, project_title(&s->projects[i]), i == s->chosen, true);
        int chosen = popup(s, m, r);
        if (chosen > 0 && (size_t)chosen - 1 != s->chosen) { s->chosen = (size_t)chosen - 1; load_choices(s); pane_relayout(s->base.pane); pane_footer_changed(s->base.pane); }
        break;
    }
    case CHIP_BRANCH: {
        HMENU m = CreatePopupMenu();
        char *first = no_branch_label(s);
        append(m, 1, first, s->branch == NULL, true); free(first);
        if (s->branch_count) AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        for (size_t i = 0; i < s->branch_count && i < 60; i++) append(m, (UINT)i + 2, s->branches[i], str_eq(s->branch, s->branches[i]), true);
        int chosen = popup(s, m, r);
        if (chosen == 1) set_string(&s->branch, NULL);
        else if (chosen >= 2 && (size_t)chosen - 2 < s->branch_count) set_string(&s->branch, s->branches[chosen - 2]);
        pane_footer_changed(s->base.pane);
        break;
    }
    case CHIP_PROVIDER: case CHIP_MODEL: {
        if (!s->has_catalog) break;
        HMENU m = CreatePopupMenu();
        UINT id = 1;
        struct { int provider; const char *model; } rows[128]; size_t n = 0;
        if (chip == CHIP_PROVIDER) {
            for (size_t p = 0; p < s->catalog.provider_count && n < 128; p++) {
                const RuntimeProvider *pr = &s->catalog.providers[p];
                char *label = runtime_provider_available(pr) ? xstrdup(pr->label) : xstrfmt("%s (unavailable)", pr->label);
                append(m, id++, label, has && eff.provider_id == pr->id, runtime_provider_available(pr)); free(label);
                rows[n].provider = pr->id; rows[n].model = pr->default_model ? pr->default_model : (pr->model_count ? pr->models[0].id : NULL); n++;
            }
        } else {
            const RuntimeProvider *pr = has ? runtime_catalog_provider(&s->catalog, eff.provider_id) : NULL;
            for (size_t k = 0; pr && k < pr->model_count && n < 128; k++) {
                append(m, id++, runtime_model_title(&pr->models[k]), has && str_eq(eff.model, pr->models[k].id), true);
                rows[n].provider = pr->id; rows[n].model = pr->models[k].id; n++;
            }
        }
        int chosen = popup(s, m, r);
        if (chosen >= 1 && (size_t)chosen - 1 < n) {
            RuntimeChoice c;
            if (runtime_catalog_choice(&s->catalog, rows[chosen - 1].provider, rows[chosen - 1].model, &c)) { if (s->has_runtime) runtime_choice_free(&s->runtime); s->runtime = c; s->has_runtime = true; remember(s); }
        }
        pane_footer_changed(s->base.pane);
        break;
    }
    case CHIP_EFFORT: {
        if (!has) break;
        size_t n; const char *const *efforts = runtime_catalog_efforts(&s->catalog, &eff, &n);
        HMENU m = CreatePopupMenu();
        for (size_t i = 0; i < n; i++) append(m, (UINT)i + 1, efforts[i], str_eq(eff.effort, efforts[i]), true);
        int chosen = popup(s, m, r);
        if (chosen >= 1 && (size_t)chosen - 1 < n) {
            RuntimeChoice c = { eff.provider_id, xstrdup(eff.model), xstrdup(efforts[chosen - 1]) };
            if (s->has_runtime) runtime_choice_free(&s->runtime);
            s->runtime = c; s->has_runtime = true; remember(s);
        }
        pane_footer_changed(s->base.pane);
        break;
    }
    case CHIP_LOOP: s->review_loop = !s->review_loop; pane_footer_changed(s->base.pane); break;
    }
    if (has) runtime_choice_free(&eff);
}
static void new_session_footer_click(Screen *base, POINT pt) {
    NewSessionScreen *s = (NewSessionScreen *)base;
    for (int c = 0; c < CHIP_COUNT; c++) if (!IsRectEmpty(&s->chip_rc[c]) && PtInRect(&s->chip_rc[c], pt)) { pick_chip(s, c); return; }
    if (PtInRect(&s->send_rc, pt)) { start(s); return; }
    if (!s->busy && attach_list_click(s->files, pt)) return;
    if (PtInRect(&s->attach_rc, pt)) { if (!s->busy) attach_list_pick(s->files); return; }
    if (PtInRect(&s->mic_rc, pt) && s->voice) {
        VoiceState vs = voice_state(s->voice);
        if (vs == VOICE_RECORDING) voice_stop(s->voice); else if (vs == VOICE_IDLE) voice_record(s->voice);
        pane_footer_changed(base->pane);
        return;
    }
    if (PtInRect(&s->box_rc, pt)) SetFocus(s->composer);
}
static void new_session_command(Screen *base, int id, int code, HWND control) {
    NewSessionScreen *s = (NewSessionScreen *)base;
    if (id == ID_COMPOSER && code == EN_CHANGE) {
        int lines = (int)SendMessageW(control, EM_GETLINECOUNT, 0, 0);
        if (lines != s->composer_lines) { s->composer_lines = lines; pane_footer_changed(base->pane); }
        else { RECT rc; GetClientRect(pane_hwnd(base->pane), &rc); rc.top = rc.bottom - new_session_footer_height(base, rc.right); InvalidateRect(pane_hwnd(base->pane), &rc, FALSE); }
    }
}

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
    const Project *p = project(s);
    char *keep = xstrdup(p ? p->repo : s->asked ? s->asked : "");
    size_t chosen = 0;
    for (size_t i = 0; i < n; i++) if (str_eq(items[i].repo, keep)) chosen = i;
    free(keep);
    projects_free(s->projects, s->count);
    s->projects = items; s->count = n; s->chosen = chosen;
    load_choices(s);
    pane_relayout(s->base.pane); pane_footer_changed(s->base.pane);
}
static void new_session_visible(Screen *base, bool shown) {
    NewSessionScreen *s = (NewSessionScreen *)base;
    if (shown) {
        ShowWindow(s->composer, SW_SHOW);
        if (!s->count && !s->req_projects) store_call("projects", json_object(), 0, s, projects_done, 0, &s->req_projects);
        if (s->count && !s->has_catalog && !s->req_runtimes) load_choices(s);
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
    const Project *p = project(s);
    if (p && str_eq(p->repo, fresh->asked)) return;
    size_t i = 0;
    while (i < s->count && !str_eq(s->projects[i].repo, fresh->asked)) i++;
    if (i == s->count) {
        const Project *wanted = project(fresh);
        if (!wanted || !str_eq(wanted->repo, fresh->asked)) return;
        s->projects = xrealloc(s->projects, (s->count + 1) * sizeof *s->projects);
        project_copy(&s->projects[s->count++], wanted);
    }
    s->chosen = i;
    set_string(&s->error, NULL); s->uncertain = false;
    load_choices(s);
    pane_relayout(base->pane); pane_footer_changed(base->pane);
}
static const ScreenVTable new_session_vt = {
    .destroy = new_session_destroy, .layout = new_session_layout, .header = new_session_header,
    .footer_height = new_session_footer_height, .footer_layout = new_session_footer_layout, .footer_paint = new_session_footer_paint,
    .footer_click = new_session_footer_click, .visible = new_session_visible, .command = new_session_command, .timer = new_session_timer,
    .activated = new_session_activated, .adopt = new_session_adopt,
};
Screen *new_session_screen_new(const Project *project_, const Project *projects, size_t count) {
    NewSessionScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &new_session_vt; s->base.id = xstrdup("new-session");
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
    s->review_loop = true;
    s->composer_lines = 1;
    HWND parent = pane_hwnd(app_detail_pane());
    s->composer = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)ID_COMPOSER, GetModuleHandleW(NULL), NULL);
    SendMessageW(s->composer, WM_SETFONT, (WPARAM)font(FONT_BODY), TRUE);
    SendMessageW(s->composer, EM_SETCUEBANNER, TRUE, (LPARAM)L"Describe what to build\x2026");
    SetWindowSubclass(s->composer, composer_proc, ID_COMPOSER, (DWORD_PTR)s);
    theme_apply_control(s->composer);
    DragAcceptFiles(s->composer, TRUE);
    s->files = attach_list_new(&s->base, "start_session");
    if (store_can_transcribe()) s->voice = voice_new(voice_changed, s);
    return &s->base;
}
