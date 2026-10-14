#include "screen_panel_internal.h"

typedef struct { char *label; COLORREF color; } SevData;

static void section_label(Doc *doc, int w, const char *text, int fold_action, bool open);
static void sev_free(void *p);
static void paint_sev(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static int doc_severity(Doc *doc, int x, int y, const char *severity);

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

void screen_panel_pr_layout(PanelScreen *s, Doc *doc, int w) {
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
