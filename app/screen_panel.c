// The dashboard's `#pr-panel`: the column on the right of a conversation with its pull request, commits, reviews and
// findings, each finding with its verdict buttons.
#include "screens.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ACT_OPEN_PR = 1000, ACT_VIEW_PR, ACT_COMMITS, ACT_FINDINGS, ACT_COMMIT_URL, ACT_FINDING_URL, ACT_DECIDE };
enum { TIMER_POLL = 1 };
enum { TAG_PULL = 1, TAG_FINDINGS, TAG_DECIDE };

typedef struct {
    Screen base;
    Project project; int number;
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
    if (!req->ok) { char *t = request_error_text(req); set_string(&s->error, t); free(t); panel_finish(s); return; }
    json_free(s->pr); s->pr = json_clone(json_get(req->result, "pr"));
    set_string(&s->error, NULL);
    if (store_supports("findings")) {
        Json *args = json_object(); json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->number);
        store_call("findings", args, 0, s, findings_done, TAG_FINDINGS, &s->req_findings);
    }
    panel_finish(s);
}
static void panel_load(PanelScreen *s) {
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
    else { char *t = request_error_text(req); set_string(&s->error, t); free(t); }
    pane_relayout(s->base.pane);
}
static void decide(PanelScreen *s, const char *key, const char *decision) {
    if (s->deciding) return;
    set_string(&s->deciding, key);
    Json *args = json_object();
    json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->number); json_set_str(args, "key", key);
    json_object_set(args, "decision", decision ? json_string(decision) : json_null());
    store_call("finding_decision", args, 0, s, decide_done, TAG_DECIDE, &s->req_decide);
    pane_relayout(s->base.pane);
}

static void panel_destroy(Screen *base) {
    PanelScreen *s = (PanelScreen *)base;
    request_cancel(&s->req_pull); request_cancel(&s->req_findings); request_cancel(&s->req_decide); poller_stop(&s->poller);
    project_free(&s->project); json_free(s->pr); json_free(s->findings); free(s->error); free(s->deciding);
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
static const char *severity_label(const char *severity, COLORREF *color) {
    char *f = str_fold(severity);
    const char *label;
    if (str_eq(f, "critical")) { label = "CRIT"; *color = theme.danger; }
    else if (str_eq(f, "high")) { label = "HIGH"; *color = theme.danger; }
    else if (str_eq(f, "low")) { label = "LOW"; *color = theme.muted; }
    else { label = "MED"; *color = theme.warn; }
    free(f);
    return label;
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
    d->label = xstrdup(severity_label(severity, &d->color));
    int w = px(4) * 2 + text_width(doc->cv, d->label, FONT_TINY_SEMIBOLD) + 2, h = px(16);
    RECT rc = { x, y, x + w, y + h };
    int i = doc_add(doc, &rc, paint_sev);
    doc_item(doc, i)->data = d; doc_item(doc, i)->free_data = sev_free;
    return w;
}
static const char *const decision_ids[] = { "fix", "optional", "dismissed" };
static const char *const decision_titles[] = { "Fix", "Optional", "Dismiss" };

static void panel_layout(Screen *base, Doc *doc) {
    PanelScreen *s = (PanelScreen *)base;
    int w = doc->width;
    doc_space(doc, px(14));
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
                    int selected = -1;
                    for (int k = 0; k < 3; k++) if (str_eq(decision, decision_ids[k])) selected = k;
                    doc_segments(doc, 0, w, decision_titles, 3, selected, ACT_DECIDE, (intptr_t)(i * 4), s->deciding == NULL);
                }
            }
        }
    }
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
        if (key && d >= 0 && d < 3) decide(s, key, str_eq(current, decision_ids[d]) ? NULL : decision_ids[d]);
        break;
    }
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
Screen *pull_panel_screen_new(const char *repo, int number) {
    PanelScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &panel_vt; s->base.id = xstrfmt("panel:%s#%d", repo, number);
    s->project.repo = xstrdup(repo); s->number = number;
    s->pr = json_null(); s->findings = json_array();
    s->findings_open = true;
    return &s->base;
}
