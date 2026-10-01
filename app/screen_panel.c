// The dashboard's `#pr-panel`: the column on the right of a conversation with its pull request, commits, reviews and
// findings, each finding with its verdict buttons, and under them the session's context usage.
#include "screens.h"
#include "canvas.h"
#include "dialogs.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ACT_OPEN_PR = 1000, ACT_VIEW_PR, ACT_COMMITS, ACT_FINDINGS, ACT_COMMIT_URL, ACT_FINDING_URL, ACT_DECIDE,
    ACT_AUTO_COMPACT, ACT_INSTRUCTIONS, ACT_COMPACT, ACT_CLEAR,
};
enum { TIMER_POLL = 1 };
enum { TAG_PULL = 1, TAG_FINDINGS, TAG_DECIDE };

typedef struct {
    Screen base;
    Session session;
    Project project; int number;   // number 0 until the session has a pull request
    Json *pr, *findings;
    char *error, *deciding;
    Request *req_pull, *req_findings, *req_decide;
    Poller poller;
    bool commits_open, findings_open;
} PanelScreen;

static void panel_save(PanelScreen *s) {
    if (json_is_null(s->pr)) return;
    Json *saved = json_object();
    json_object_set(saved, "pr", json_clone(s->pr));
    json_object_set(saved, "findings", json_clone(s->findings));
    json_object_set(saved, "row", json_null());
    char *key = xstrfmt("pull:%s#%d", s->project.repo, s->number);
    Json *old = cache_value(g_store.cache, key);
    if (old && !json_is_null(json_get(old, "row"))) json_object_set(saved, "row", json_clone(json_get(old, "row")));
    json_free(old);
    cache_store(g_store.cache, saved, key);
    json_free(saved); free(key);
}
static void panel_finish(PanelScreen *s) {
    if (s->req_pull || s->req_findings) return;
    panel_save(s);
    poller_finished(&s->poller, s->error != NULL, -1);
    pane_relayout(s->base.pane);
}
static void findings_done(void *owner, Request *req) {
    PanelScreen *s = owner;
    if (req->ok) { json_free(s->findings); s->findings = json_clone(json_get(req->result, "findings")); }
    panel_finish(s);
}
static void pull_done(void *owner, Request *req) {
    PanelScreen *s = owner;
    if (!req->ok) { request_error_into(&s->error, req); panel_finish(s); return; }
    json_free(s->pr); s->pr = json_clone(json_get(req->result, "pr"));
    set_string(&s->error, NULL);
    if (store_supports("findings")) {
        Json *args = json_object(); json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->number);
        store_call("findings", args, 0, s, findings_done, TAG_FINDINGS, &s->req_findings);
    }
    panel_finish(s);
}
static void panel_load(PanelScreen *s) {
    if (!s->number) { poller_finished(&s->poller, false, -1); return; }
    if (json_is_null(s->pr)) {
        char *key = xstrfmt("pull:%s#%d", s->project.repo, s->number);
        Json *saved = cache_value(g_store.cache, key);
        if (saved) { json_free(s->pr); s->pr = json_clone(json_get(saved, "pr")); json_free(s->findings); s->findings = json_clone(json_get(saved, "findings")); json_free(saved); pane_relayout(s->base.pane); }
        free(key);
    }
    if (s->req_pull) return;
    Json *args = json_object(); json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->number);
    store_call("pull", args, 0, s, pull_done, TAG_PULL, &s->req_pull);
}
static void decide_done(void *owner, Request *req) {
    PanelScreen *s = owner;
    set_string(&s->deciding, NULL);
    if (req->ok) { json_free(s->findings); s->findings = json_clone(json_get(req->result, "findings")); panel_save(s); }
    else { request_error_into(&s->error, req); }
    pane_relayout(s->base.pane);
}
static void decide(PanelScreen *s, const char *key, const char *decision) {
    if (s->deciding) return;
    set_string(&s->deciding, key);
    Json *args = json_object();
    json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->number); json_set_str(args, "key", key);
    json_object_set(args, "decision", json_string_or_null(decision));
    store_call("finding_decision", args, 0, s, decide_done, TAG_DECIDE, &s->req_decide);
    pane_relayout(s->base.pane);
}

static void panel_destroy(Screen *base) {
    PanelScreen *s = (PanelScreen *)base;
    request_cancel(&s->req_pull); request_cancel(&s->req_findings); request_cancel(&s->req_decide); poller_stop(&s->poller);
    session_free(&s->session); project_free(&s->project); json_free(s->pr); json_free(s->findings); free(s->error); free(s->deciding);
    screen_release(base);
}

// MARK: - Layout

/// `text-[12px] tracking-wide text-muted`, the panel's section labels; the folding ones carry ▸/▾.
static void section_label(Doc *doc, int w, const char *text, int fold_action, bool open) {
    doc_space(doc, px(10)); doc_rule(doc, 0, w); doc_space(doc, px(10));
    char *label = fold_action ? xstrfmt("%s %s", open ? "\xE2\x96\xBE" : "\xE2\x96\xB8", text) : xstrdup(text);
    int i = doc_text(doc, 0, w, label, FONT_CAPTION, theme.muted, DT_SINGLELINE | DT_END_ELLIPSIS);
    if (fold_action) { doc_item(doc, i)->action = fold_action; doc_item(doc, i)->hand = true; }
    free(label);
    doc_space(doc, px(4));
}
typedef struct { char *label; COLORREF color; } SevData;
static void sev_free(void *p) { SevData *d = p; free(d->label); free(d); }
static void paint_sev(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    SevData *d = it->data;
    fill_round_rect(cv, rc, px(4), theme.sidebar, d->color == theme.muted ? theme.line : d->color);
    RECT t = *rc;
    draw_text(cv, d->label, &t, FONT_TINY_SEMIBOLD, d->color, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
/// `rounded-[4px] border px-1 text-[10px] font-semibold`; returns its width.
static int doc_severity(Doc *doc, int x, int y, const char *severity) {
    SevData *d = xcalloc(1, sizeof *d);
    d->label = xstrdup(finding_severity_label(severity, &d->color));
    int w = px(4) * 2 + text_width(doc->cv, d->label, FONT_TINY_SEMIBOLD) + 2, h = px(16);
    RECT rc = { x, y, x + w, y + h };
    int i = doc_add(doc, &rc, paint_sev);
    doc_item(doc, i)->data = d; doc_item(doc, i)->free_data = sev_free;
    return w;
}

static void pr_layout(PanelScreen *s, Doc *doc, int w) {
    doc_text(doc, 0, w, "Pull request", FONT_CAPTION, theme.muted, DT_SINGLELINE);
    doc_space(doc, px(12));
    if (s->error && json_is_null(s->pr)) { doc_notice(doc, 0, w, s->error); doc_space(doc, px(8)); }
    if (json_is_null(s->pr)) { if (!s->error) doc_loading(doc, 0, w, "Loading\xE2\x80\xA6"); return; }
    const Json *pr = s->pr;
    const char *title = json_str(json_get(pr, "title"));
    char *fallback = xstrfmt("Pull request #%d", s->number);
    int ti = doc_text(doc, 0, w, title ? title : fallback, FONT_BODY_SEMIBOLD, theme.ink, DT_WORDBREAK);
    free(fallback);
    if (safe_web_url(json_str(json_get(pr, "url")))) { doc_item(doc, ti)->action = ACT_OPEN_PR; doc_item(doc, ti)->hand = true; }
    doc_space(doc, px(6));
    // `#70` and the state tag.
    char *number = xstrfmt("#%d", s->number);
    int nw = text_width(doc->cv, number, FONT_CAPTION);
    int y = doc->y;
    RECT nr = { 0, y, nw + 2, y + px(20) };
    int ni = doc_text_at(doc, &nr, number, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (safe_web_url(json_str(json_get(pr, "url")))) { doc_item(doc, ni)->action = ACT_OPEN_PR; doc_item(doc, ni)->hand = true; }
    free(number);
    const char *state = json_str(json_get(pr, "state"));
    bool draft = json_bool_is(json_get(pr, "draft"), true);
    const char *state_text = draft && str_eq(state, "open") ? "draft" : state ? state : "open";
    COLORREF state_color = str_eq(state, "merged") ? theme.accent : str_eq(state, "closed") ? theme.danger : draft ? theme.muted : theme.ok;
    doc->y = y + px(1);
    doc_badges(doc, nw + px(8), w - nw - px(8), &(BadgeSpec){ 0, state_text, state_color, false }, 1, theme.sidebar);
    doc->y = y + px(20);
    // `+1579 −129 · 8 files · 11 commits`
    double add = 0, del = 0, files = 0, commits = 0;
    bool has_add = json_num(json_get(pr, "additions"), &add), has_del = json_num(json_get(pr, "deletions"), &del);
    json_num(json_get(pr, "changedFiles"), &files); json_num(json_get(pr, "commits"), &commits);
    if (has_add || has_del) {
        int x = 0; y = doc->y;
        char *a = xstrfmt("+%d", (int)add), *d = xstrfmt("\xE2\x88\x92%d", (int)del);
        char *rest = xstrfmt(" \xC2\xB7 %d files \xC2\xB7 %d commits", (int)files, (int)commits);
        int aw = text_width(doc->cv, a, FONT_CAPTION), dw = text_width(doc->cv, d, FONT_CAPTION);
        RECT ar = { x, y, x + aw + 2, y + px(20) }; doc_text_at(doc, &ar, a, FONT_CAPTION, theme.ok, DT_LEFT | DT_VCENTER | DT_SINGLELINE); x += aw + px(4);
        RECT dr = { x, y, x + dw + 2, y + px(20) }; doc_text_at(doc, &dr, d, FONT_CAPTION, theme.danger, DT_LEFT | DT_VCENTER | DT_SINGLELINE); x += dw;
        RECT rr = { x, y, w, y + px(20) }; doc_text_at(doc, &rr, rest, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        doc->y = y + px(20);
        free(a); free(d); free(rest);
    }
    doc_space(doc, px(6));
    doc_button(doc, 0, 0, "View PR", BUTTON_BORDERED, ACT_VIEW_PR, 0, true);
    // Commits (N)
    const Json *commit_list = json_get(pr, "commitList");
    if (json_count(commit_list)) {
        char *label = xstrfmt("Commits (%d)", (int)(commits > 0 ? commits : (double)json_count(commit_list)));
        section_label(doc, w, label, ACT_COMMITS, s->commits_open);
        free(label);
        if (s->commits_open) {
            for (size_t i = 0; i < json_count(commit_list); i++) {
                const Json *cm = json_at(commit_list, i);
                const char *sha = json_str(json_get(cm, "sha")); char *short_sha = xstrndup(sha ? sha : "", sha && strlen(sha) > 7 ? 7 : (sha ? strlen(sha) : 0));
                int sw = text_width(doc->cv, short_sha, FONT_MONO_CAPTION2);
                y = doc->y;
                RECT sr = { 0, y, sw + 2, y + px(20) }; doc_text_at(doc, &sr, short_sha, FONT_MONO_CAPTION2, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                const char *message = json_str(json_get(cm, "message"));
                RECT mr = { sw + px(6), y, w, y + px(20) };
                int mi = doc_text_at(doc, &mr, message ? message : "", FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                if (safe_web_url(json_str(json_get(cm, "url")))) { doc_item(doc, mi)->action = ACT_COMMIT_URL; doc_item(doc, mi)->arg = (intptr_t)i; doc_item(doc, mi)->hand = true; }
                doc->y = y + px(20);
                free(short_sha);
            }
        }
    }
    // Reviews
    section_label(doc, w, "Reviews", 0, false);
    const Json *reviews = json_get(pr, "reviews");
    for (size_t i = 0; i < json_count(reviews); i++) {
        const Json *r = json_at(reviews, i);
        const char *st = json_str(json_get(r, "state"));
        const char *mark = str_eq(st, "approved") ? "\xE2\x9C\x93" : str_eq(st, "changes_requested") ? "\xE2\x9C\x97" : "\xE2\x97\x8B";
        COLORREF mc = str_eq(st, "approved") ? theme.ok : str_eq(st, "changes_requested") ? theme.danger : theme.muted;
        char *spaced = str_replace(st ? st : "", "_", " ");
        char *line = xstrfmt("%s %s", mark, spaced);
        int li = doc_text(doc, 0, w, line, FONT_CAPTION, theme.muted, DT_SINGLELINE | DT_END_ELLIPSIS);
        (void)mc; (void)li;
        free(line); free(spaced);
        doc_space(doc, px(2));
    }
    if (!json_count(reviews)) doc_text(doc, 0, w, "\xE2\x97\x8B none yet", FONT_CAPTION, theme.muted, DT_SINGLELINE);
    // Findings (N · all fixed)
    if (store_supports("findings")) {
        size_t fn = json_count(s->findings), fixed = 0;
        for (size_t i = 0; i < fn; i++) if (json_bool_is(json_get(json_at(s->findings, i), "fixed"), true)) fixed++;
        char *label = fn ? (fixed == fn ? xstrfmt("Findings (%zu \xC2\xB7 all fixed)", fn) : fixed ? xstrfmt("Findings (%zu \xC2\xB7 %zu fixed)", fn, fixed) : xstrfmt("Findings (%zu)", fn)) : xstrdup("Findings");
        section_label(doc, w, label, ACT_FINDINGS, s->findings_open);
        free(label);
        if (s->findings_open) {
            if (!fn) doc_text(doc, 0, w, "No findings reported", FONT_CAPTION, theme.muted, DT_SINGLELINE);
            for (size_t i = 0; i < fn; i++) {
                const Json *f = json_at(s->findings, i);
                if (i) doc_space(doc, px(8));
                y = doc->y;
                int sw = doc_severity(doc, 0, y + px(2), json_str(json_get(f, "severity")));
                bool is_fixed = json_bool_is(json_get(f, "fixed"), true);
                const char *decision = json_str(json_get(f, "decision"));
                const char *right = is_fixed ? "\xE2\x9C\x93 fixed" : "";
                int rw = *right ? text_width(doc->cv, right, FONT_CAPTION2) + px(6) : 0;
                const char *ft = json_str(json_get(f, "title"));
                RECT tr = { sw + px(6), y, w - rw, y + px(20) };
                int fi = doc_text_at(doc, &tr, ft ? ft : "Finding", FONT_CAPTION, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                if (safe_web_url(json_str(json_get(f, "url")))) { doc_item(doc, fi)->action = ACT_FINDING_URL; doc_item(doc, fi)->arg = (intptr_t)i; doc_item(doc, fi)->hand = true; }
                if (rw) { RECT rr = { w - rw, y, w, y + px(20) }; doc_text_at(doc, &rr, right, FONT_CAPTION2, theme.ok, DT_RIGHT | DT_VCENTER | DT_SINGLELINE); }
                doc->y = y + px(20) + px(4);
                if (store_supports("finding_decision") && json_str(json_get(f, "key")) && store_can_manage()) {
                    int selected = finding_decision_index(decision);
                    doc_segments(doc, 0, w, finding_decision_titles, FINDING_DECISION_COUNT, selected, ACT_DECIDE, (intptr_t)(i * 4), s->deciding == NULL);
                }
            }
        }
    }
}

// MARK: - Context usage

/// The bar's colours, keyed on the category names claude's /context report uses; deferred and buffer rows and unknown
/// categories go grey, free space the near-background grey of the track.
static COLORREF context_color(const char *name) {
    static const struct { const char *name; COLORREF color; } colors[] = {
        { "messages", RGB(0x6d, 0x9e, 0xf7) }, { "system prompt", RGB(0xe0, 0x6c, 0x75) }, { "system tools", RGB(0xd9, 0x8a, 0x3f) },
        { "mcp tools", RGB(0x4f, 0xae, 0x72) }, { "skills", RGB(0xc9, 0x6f, 0x9e) }, { "memory files", RGB(0x5f, 0xae, 0x5f) },
        { "custom agents", RGB(0x8f, 0x7e, 0xe8) }, { "context", RGB(0x6d, 0x9e, 0xf7) },
    };
    char *n = str_fold(name ? name : "");
    COLORREF color = RGB(0x8a, 0x86, 0x7c);
    if (str_eq(n, "free space")) color = theme.line;
    else if (strstr(n, "deferred") || strstr(n, "autocompact")) color = RGB(0x55, 0x52, 0x4c);
    else for (size_t i = 0; i < sizeof colors / sizeof *colors; i++) if (str_eq(n, colors[i].name)) { color = colors[i].color; break; }
    free(n);
    return color;
}
typedef struct { size_t count; COLORREF *colors; double *pct; } BarData;
static void bar_free(void *p) { BarData *d = p; free(d->colors); free(d->pct); free(d); }
/// `flex h-1.5 overflow-hidden rounded-full bg-sunken`, one segment per category.
static void paint_bar(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    BarData *d = it->data;
    int h = rc->bottom - rc->top, w = rc->right - rc->left;
    canvas_clip_round(cv, rc, h / 2);
    fill_rect(cv, rc, theme.sunken);
    double x = rc->left;
    for (size_t i = 0; i < d->count; i++) {
        double pct = d->pct[i] < 0 ? 0 : d->pct[i] > 100 ? 100 : d->pct[i];
        double next = x + w * pct / 100;
        RECT seg = { (int)(x + 0.5), rc->top, (int)(next + 0.5), rc->bottom };
        if (seg.right > seg.left) fill_rect(cv, &seg, d->colors[i]);
        x = next;
    }
    canvas_unclip(cv);
}
/// `<input type="checkbox" class="accent-accent">`
static void paint_checkbox(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    bool on = it->arg != 0;
    fill_round_rect(cv, rc, px(3), on ? theme.accent : theme.field, on ? theme.accent : theme.line_strong);
    if (on) draw_check_mark(cv, rc, theme.on_accent);
}
static void paint_swatch(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    COLORREF color = (COLORREF)it->arg;
    fill_round_rect(cv, rc, px(3), color, color);
}
/// `flex justify-between text-[12px] text-muted`: the label, and the value in ink at the right.
static void usage_row(Doc *doc, int w, const char *label, const char *value) {
    int y = doc->y, h = px(18);
    int vw = text_width(doc->cv, value, FONT_CAPTION) + 2;
    RECT vr = { w - vw, y, w, y + h }, lr = { 0, y, w - vw - px(6), y + h };
    doc_text_at(doc, &lr, label, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    doc_text_at(doc, &vr, value, FONT_CAPTION, theme.ink, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    doc->y = y + h + px(2);
}
static bool num_at(const Json *object, const char *key, double *out) { return json_num(json_get(object, key), out); }
/// What the session spent with everything it ordered, or its own figures from a server without `usage`.
static const Json *usage_of(const Json *raw) { const Json *u = json_get(raw, "usage"); return json_is_object(u) ? u : raw; }
static bool context_size(const Json *raw, double *used, double *window) {
    const Json *cu = json_get(raw, "contextUsage");
    if (!num_at(cu, "window", window) && !num_at(raw, "contextWindow", window)) *window = 0;
    return num_at(cu, "tokens", used) || num_at(raw, "contextTokens", used);
}
static bool can_op(const char *operation) { return store_supports(operation) && store_can_manage(); }
static bool auto_compact_shown(const Json *raw) { double at; return num_at(raw, "autoCompactAt", &at) && at > 0 && can_op("rename"); }
static bool instructions_shown(const Json *raw) {
    return (json_bool_is(json_get(raw, "compactTakesInstructions"), true) || json_str_nonempty(json_get(raw, "compactInstructions"))) && can_op("rename");
}
static bool compact_shown(const Json *raw) {
    return (json_bool_is(json_get(raw, "canCompact"), true) || json_bool_is(json_get(raw, "compacting"), true)) && can_op("compact");
}
static bool usage_rows_present(const Json *raw) {
    const Json *u = usage_of(raw);
    static const char *const keys[] = { "inputTokens", "outputTokens", "durationMs", "costUsd" };
    double v;
    for (size_t i = 0; i < 4; i++) if (num_at(u, keys[i], &v)) return true;
    return false;
}
bool session_panel_wanted(const Session *session) {
    if (session_pull_number(session) && session_repo(session) && store_supports("pull")) return true;
    const Json *raw = session->raw;
    double used, window;
    return context_size(raw, &used, &window) || usage_rows_present(raw) || compact_shown(raw) || auto_compact_shown(raw) || instructions_shown(raw);
}
/// An ISO time as the transcript words it ("2:13 PM", or with the day when not today).
static char *usage_time(const Json *value) {
    time_t when;
    if (!board_date_parse(json_str(value), &when)) return NULL;
    return format_event_time(when);
}

/// The dashboard's `usageSection`: how much of the model's window is used, as a bar split into claude's /context
/// categories when the server has them and used-vs-free otherwise, then what the session consumed, and the compaction
/// controls.
static void usage_layout(PanelScreen *s, Doc *doc, int w, bool after_pr) {
    const Json *raw = s->session.raw, *cu = json_get(raw, "contextUsage"), *u = usage_of(raw);
    double used = 0, window = 0, v;
    bool has_context = context_size(raw, &used, &window);
    bool active = session_is_active(&s->session), compacting = json_bool_is(json_get(raw, "compacting"), true);
    bool clear = can_op("clear") && str_eq(json_str(json_get(raw, "kind")), "devchat") && !active && !compacting;
    bool auto_shown = auto_compact_shown(raw), keep_shown = instructions_shown(raw), compact = compact_shown(raw);
    if (!has_context && !usage_rows_present(raw) && !compact && !auto_shown && !keep_shown) return;

    if (after_pr) { doc_space(doc, px(10)); doc_rule(doc, 0, w); doc_space(doc, px(10)); }
    // "Context usage", with the Auto-compact checkbox at the right of the same line.
    int y = doc->y, h = px(20);
    int label_right = w;
    if (auto_shown) {
        double at = 0; num_at(raw, "autoCompactAt", &at);
        char *label = xstrfmt("Auto-compact %dk", (int)(at / 1000 + 0.5));
        bool on = json_bool_is(json_get(raw, "autoCompact"), true);
        int lw = text_width(doc->cv, label, FONT_CAPTION) + 2, box = px(12);
        int x = w - lw - box - px(5);
        RECT br = { x, y + (h - box) / 2, x + box, y + (h - box) / 2 + box };
        int bi = doc_add(doc, &br, paint_checkbox);
        doc_item(doc, bi)->arg = on; doc_item(doc, bi)->action = ACT_AUTO_COMPACT; doc_item(doc, bi)->hand = true;
        RECT lr = { w - lw, y, w, y + h };
        int li = doc_text_at(doc, &lr, label, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        doc_item(doc, li)->action = ACT_AUTO_COMPACT; doc_item(doc, li)->hand = true;
        label_right = x - px(6);
        free(label);
    }
    RECT hr = { 0, y, label_right, y + h };
    doc_text_at(doc, &hr, "Context usage", FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    doc->y = y + h + px(4);
    // Instructions (accented by a ✓ while some are set), Compact and Clear.
    ButtonSpec buttons[3]; size_t nb = 0;
    if (keep_shown) {
        bool set = json_str_nonempty(json_get(raw, "compactInstructions")) != NULL;
        buttons[nb++] = (ButtonSpec){ 0, set ? "Instructions \xE2\x9C\x93" : "Instructions", BUTTON_BORDERED, ACT_INSTRUCTIONS, 0, true };
    }
    if (compact) buttons[nb++] = (ButtonSpec){ 0, compacting ? "Compacting\xE2\x80\xA6" : "Compact", BUTTON_BORDERED, ACT_COMPACT, 0, !compacting };
    // Takes the transcript off the screen only: the agent's context and the stored log stay as they are.
    if (clear) buttons[nb++] = (ButtonSpec){ 0, "Clear", BUTTON_BORDERED, ACT_CLEAR, 0, true };
    if (nb) { doc_button_row(doc, 0, w, buttons, nb); doc_space(doc, px(8)); }

    if (has_context) {
        char *u_text = format_tokens(used);
        char *headline;
        if (window > 0) { char *w_text = format_tokens(window); headline = xstrfmt("%s / %s (%d%%)", u_text, w_text, (int)(used / window * 100 + 0.5)); free(w_text); }
        else headline = xstrfmt("%s tokens", u_text);
        free(u_text);
        usage_row(doc, w, "Context", headline);
        free(headline);
        // Categories come from the claude /context probe; the other providers still get a bar, just an undivided one.
        const Json *cats = json_get(cu, "categories");
        size_t nc = 0;
        for (size_t i = 0; i < json_count(cats); i++) if (num_at(json_at(cats, i), "tokens", &v)) nc++;
        BarData *bar = xcalloc(1, sizeof *bar);
        if (nc) {
            bar->colors = xcalloc(nc, sizeof *bar->colors); bar->pct = xcalloc(nc, sizeof *bar->pct);
            for (size_t i = 0; i < json_count(cats); i++) {
                const Json *c = json_at(cats, i);
                if (!num_at(c, "tokens", &v)) continue;
                bar->colors[bar->count] = context_color(json_str(json_get(c, "name")));
                bar->pct[bar->count++] = json_num_or(json_get(c, "pct"), 0);
            }
        } else if (window > 0) {
            bar->colors = xcalloc(2, sizeof *bar->colors); bar->pct = xcalloc(2, sizeof *bar->pct);
            bar->colors[0] = context_color("context"); bar->pct[0] = used / window * 100;
            bar->colors[1] = context_color("free space"); bar->pct[1] = 100 - used / window * 100;
            bar->count = 2;
        }
        if (bar->count) { doc_space(doc, px(2)); doc_custom(doc, 0, w, px(6), paint_bar, bar, bar_free, 0, 0); doc_space(doc, px(6)); }
        else bar_free(bar);
        for (size_t i = 0; i < json_count(cats); i++) {
            const Json *c = json_at(cats, i);
            if (!num_at(c, "tokens", &v)) continue;
            const char *name = json_str(json_get(c, "name"));
            char *tokens = format_tokens(v), *pct = xstrfmt("%.1f%%", json_num_or(json_get(c, "pct"), 0));
            int y = doc->y, h = px(18), sq = px(8);
            int pw = px(44), tw = text_width(doc->cv, tokens, FONT_CAPTION) + 2;
            RECT sw = { 0, y + (h - sq) / 2, sq, y + (h - sq) / 2 + sq };
            int si = doc_add(doc, &sw, paint_swatch); doc_item(doc, si)->arg = (intptr_t)context_color(name);
            RECT nr = { sq + px(6), y, w - pw - tw - px(6), y + h };
            doc_text_at(doc, &nr, name ? name : "", FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            RECT tr = { w - pw - tw, y, w - pw, y + h };
            doc_text_at(doc, &tr, tokens, FONT_CAPTION, theme.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            RECT pr = { w - pw, y, w, y + h };
            doc_text_at(doc, &pr, pct, FONT_CAPTION, theme.ink, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            doc->y = y + h;
            free(tokens); free(pct);
        }
    }
    bool rows = usage_rows_present(raw) || num_at(u, "sessions", &v) || json_str(json_get(cu, "compactedAt"));
    if (!rows) return;
    if (has_context) { doc_space(doc, px(6)); doc_rule(doc, 0, w); doc_space(doc, px(6)); }
    char *t;
    if (num_at(u, "inputTokens", &v)) { t = format_tokens(v); usage_row(doc, w, "Input tokens", t); free(t); }
    if (num_at(u, "outputTokens", &v)) { t = format_tokens(v); usage_row(doc, w, "Output tokens", t); free(t); }
    if (num_at(u, "durationMs", &v)) { t = format_duration_ms(v); usage_row(doc, w, "Agent time", t); free(t); }
    if (num_at(u, "costUsd", &v)) {
        // `+`: some turns carry no price at all, so the total is a floor.
        double unpriced = 0; num_at(u, "unpricedTurns", &unpriced);
        t = xstrfmt("$%.2f%s", v, unpriced > 0 ? "+" : ""); usage_row(doc, w, "Cost", t); free(t);
    }
    // An orchestrator's figures cover its workers, a task's the reviews its loop ran.
    if (num_at(u, "sessions", &v) && v > 0) { t = xstrfmt("%d session%s it started", (int)v, (int)v == 1 ? "" : "s"); usage_row(doc, w, "Includes", t); free(t); }
    if (str_eq(json_str(json_get(cu, "source")), "codex")) {
        if (num_at(cu, "cachedInputTokens", &v)) { t = format_tokens(v); usage_row(doc, w, "Thread cached input", t); free(t); }
        if (num_at(cu, "reasoningOutputTokens", &v)) { t = format_tokens(v); usage_row(doc, w, "Thread reasoning output", t); free(t); }
        if ((t = usage_time(json_get(cu, "at")))) { usage_row(doc, w, "Context updated", t); free(t); }
    }
    if ((t = usage_time(json_get(cu, "compactedAt")))) { usage_row(doc, w, "Last compact", t); free(t); }
}

static void panel_layout(Screen *base, Doc *doc) {
    PanelScreen *s = (PanelScreen *)base;
    int w = doc->width;
    doc_space(doc, px(14));
    bool pr = s->number && store_supports("pull");
    if (pr) pr_layout(s, doc, w);
    usage_layout(s, doc, w, pr);
    doc_space(doc, px(14));
}
static void panel_header(Screen *base, HeaderInfo *info) { (void)base; (void)info; }
static void panel_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    PanelScreen *s = (PanelScreen *)base;
    switch (action) {
    case ACT_OPEN_PR: open_web_url(json_str(json_get(s->pr, "url"))); break;
    case ACT_VIEW_PR: app_push_detail(pull_detail_screen_new(&s->project, s->number, NULL, NULL)); break;
    case ACT_COMMITS: s->commits_open = !s->commits_open; pane_relayout(base->pane); break;
    case ACT_FINDINGS: s->findings_open = !s->findings_open; pane_relayout(base->pane); break;
    case ACT_COMMIT_URL: open_web_url(json_str(json_get(json_at(json_get(s->pr, "commitList"), (size_t)arg), "url"))); break;
    case ACT_FINDING_URL: open_web_url(json_str(json_get(json_at(s->findings, (size_t)arg), "url"))); break;
    case ACT_DECIDE: {
        size_t index = (size_t)(arg / 4); int d = (int)(arg % 4);
        const Json *f = json_at(s->findings, index);
        const char *key = json_str(json_get(f, "key")), *current = json_str(json_get(f, "decision"));
        // The same pick twice clears it, as the dashboard does.
        if (key && d >= 0 && d < FINDING_DECISION_COUNT) decide(s, key, str_eq(current, finding_decision_ids[d]) ? NULL : finding_decision_ids[d]);
        break;
    }
    case ACT_AUTO_COMPACT: {
        Json *extra = json_object();
        json_set_bool(extra, "autoCompact", !json_bool_is(json_get(s->session.raw, "autoCompact"), true));
        conversation_session_op(session_id(&s->session), "rename", extra);
        break;
    }
    case ACT_INSTRUCTIONS: {
        const char *current = json_str(json_get(s->session.raw, "compactInstructions"));
        char *text = dialog_text(app_window(), "Compaction instructions", "What every compaction of this session must keep (empty clears them):", "Save", current ? current : "");
        if (!text) break;
        char *trimmed = str_trim(text);
        if (!str_eq(trimmed, current ? current : "")) {
            Json *extra = json_object(); json_set_str(extra, "compactInstructions", trimmed);
            conversation_session_op(session_id(&s->session), "rename", extra);
        }
        free(trimmed); free(text);
        break;
    }
    case ACT_COMPACT:
        if (app_confirm("Compact this conversation?", "Summarizes the conversation to free context. This uses the provider and may incur usage.", "Compact", false))
            conversation_session_op(session_id(&s->session), "compact", NULL);
        break;
    case ACT_CLEAR:
        if (app_confirm("Clear the transcript?", "Hides the transcript so far from this chat. Nothing is deleted and the agent's context is unchanged.", "Clear", false))
            conversation_session_op(session_id(&s->session), "clear", NULL);
        break;
    }
}
static void panel_timer(Screen *base, UINT id) { PanelScreen *s = (PanelScreen *)base; if (poller_fired(&s->poller, id)) { if (s->deciding) poller_finished(&s->poller, false, -1); else panel_load(s); } }
static void panel_visible(Screen *base, bool shown) {
    PanelScreen *s = (PanelScreen *)base;
    if (shown) poller_start(&s->poller, base->pane, TIMER_POLL, 30000);
    else { poller_stop(&s->poller); request_cancel(&s->req_pull); request_cancel(&s->req_findings); }
}
static void panel_refresh(Screen *base) { PanelScreen *s = (PanelScreen *)base; request_cancel(&s->req_pull); panel_load(s); }
static const ScreenVTable panel_vt = {
    .destroy = panel_destroy, .layout = panel_layout, .header = panel_header, .action = panel_action, .timer = panel_timer,
    .visible = panel_visible, .refresh = panel_refresh,
};
/// The pull request the panel shows, or none; a session that links one later gets its sections then.
static void panel_set_pull(PanelScreen *s, const char *repo, int number) {
    if (!repo || !store_supports("pull")) number = 0;
    if (number == s->number) return;
    request_cancel(&s->req_pull); request_cancel(&s->req_findings);
    project_free(&s->project); memset(&s->project, 0, sizeof s->project);
    s->project.repo = xstrdup(repo ? repo : "");
    s->number = number;
    json_free(s->pr); s->pr = json_null(); json_free(s->findings); s->findings = json_array();
    set_string(&s->error, NULL);
}
Screen *session_panel_screen_new(const Session *session) {
    PanelScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &panel_vt; s->base.id = xstrfmt("panel:%s", session_id(session));
    session_copy(&s->session, session);
    s->project.repo = xstrdup(session_repo(session) ? session_repo(session) : "");
    s->pr = json_null(); s->findings = json_array();
    s->findings_open = true;
    panel_set_pull(s, session_repo(session), session_pull_number(session));
    return &s->base;
}
void session_panel_update(const Session *session) {
    Screen *root = app_panel_pane() ? pane_root(app_panel_pane()) : NULL;
    char *id = xstrfmt("panel:%s", session_id(session));
    bool ours = root && root->id && str_eq(root->id, id);
    free(id);
    if (!ours) return;
    PanelScreen *s = (PanelScreen *)root;
    session_free(&s->session); session_copy(&s->session, session);
    int before = s->number;
    panel_set_pull(s, session_repo(session), session_pull_number(session));
    if (s->number && s->number != before) panel_load(s);
    pane_relayout(root->pane);
}
