#include "screen_pulls_internal.h"

static bool catalog_lists(const Json *catalog, const char *id);

static const char *const decision_titles[] = { "Undecided", "Fix", "Optional", "Dismiss" };

void screen_pulls_layout_checks(PullScreen *s, Doc *doc, Col c) {
    int box = screen_pulls_col_box(doc, c);
    CountsData *cd = xcalloc(1, sizeof *cd);
    cd->passed = json_int_or(json_get(json_get(s->pr, "checks"), "passed"), 0); cd->failed = json_int_or(json_get(json_get(s->pr, "checks"), "failed"), 0); cd->pending = json_int_or(json_get(json_get(s->pr, "checks"), "pending"), 0);
    doc_custom(doc, c.ix, c.iw, px(22), screen_pulls_paint_counts, cd, free, 0, 0);
    const Json *runs = json_get(json_get(s->pr, "checks"), "runs");
    for (size_t i = 0; i < json_count(runs); i++) {
        const Json *check = json_at(runs, i);
        screen_pulls_col_gap(doc, c);
        const char *result = json_str(json_get(check, "conclusion")); if (!result) result = json_str(json_get(check, "status")); if (!result) result = "Pending";
        CheckData *d = xcalloc(1, sizeof *d);
        d->glyph = check_glyph(result, &d->color);
        const char *name = json_str(json_get(check, "name")); d->name = xstrdup(name ? name : "Check");
        char *spaced = str_replace(result, "_", " "); d->result = str_capitalized(spaced); free(spaced);
        const char *curl = json_str(json_get(check, "url"));
        doc_custom(doc, c.ix, c.iw, px(22), screen_pulls_paint_check, d, screen_pulls_check_free, safe_web_url(curl) ? ACT_CHECK_URL : 0, (intptr_t)i);
    }
    if (!json_count(runs)) { screen_pulls_col_gap(doc, c); doc_text(doc, c.ix, c.iw, "No checks reported", FONT_CALLOUT, theme.secondary, DT_SINGLELINE); }
    doc_box_end(doc, box, px(12));
}

void screen_pulls_layout_commit_list(PullScreen *s, Doc *doc, Col c) {
    const Json *commits = json_get(s->pr, "commitList");
    int box = screen_pulls_col_box(doc, c);
    for (size_t i = 0; i < json_count(commits); i++) {
        const Json *cm = json_at(commits, i);
        if (i) screen_pulls_col_gap(doc, c);
        const char *sha = json_str(json_get(cm, "sha")); char *short_sha = xstrndup(sha ? sha : "", sha && strlen(sha) > 7 ? 7 : (sha ? strlen(sha) : 0));
        int y = doc->y; int sw = text_width(doc->cv, short_sha, FONT_MONO_SMALL);
        RECT sr = { c.ix + c.iw - sw, y, c.ix + c.iw, y + px(18) }; doc_text_at(doc, &sr, short_sha, FONT_MONO_SMALL, theme.secondary, DT_RIGHT | DT_SINGLELINE);
        const char *message = json_str(json_get(cm, "message"));
        int mi = doc_label(doc, c.ix, c.iw - sw - px(12), 0xE8EE, message ? message : "", FONT_CALLOUT, theme.text);
        if (safe_web_url(json_str(json_get(cm, "url")))) { doc_item(doc, mi)->action = ACT_COMMIT_URL; doc_item(doc, mi)->arg = (intptr_t)i; doc_item(doc, mi)->hand = true; }
        free(short_sha);
    }
    if (!json_count(commits)) doc_text(doc, c.ix, c.iw, "No commits reported", FONT_CALLOUT, theme.secondary, DT_SINGLELINE);
    doc_box_end(doc, box, px(12));
}

/// The findings not yet fixed, and those among them the user marked fix.
size_t screen_pulls_findings_unfixed(PullScreen *s) {
    size_t n = 0;
    for (size_t i = 0; i < json_count(s->findings); i++) if (!json_bool_is(json_get(json_at(s->findings, i), "fixed"), true)) n++;
    return n;
}

size_t screen_pulls_findings_to_fix(PullScreen *s) {
    size_t n = 0;
    for (size_t i = 0; i < json_count(s->findings); i++) {
        const Json *f = json_at(s->findings, i);
        if (!json_bool_is(json_get(f, "fixed"), true) && str_eq(json_str(json_get(f, "decision")), "fix")) n++;
    }
    return n;
}

/// Whether the server's errand list has one; before the list is read, every errand this app knows is taken as offered.
static bool catalog_lists(const Json *catalog, const char *id) {
    if (!json_count(catalog)) return true;
    for (size_t i = 0; i < json_count(catalog); i++) if (str_eq(json_str(json_get(json_at(catalog, i), "id")), id)) return true;
    return false;
}

/// Solve findings is the board's implement-feedback errand started from the findings themselves: offered on an open pull
/// request with findings still to fix, to a token that may start errands on a server that lists this one.
bool screen_pulls_solve_findings_offered(PullScreen *s) {
    if (!store_can_manage() || !store_supports("action") || !catalog_lists(s->catalog, "implement-feedback")) return false;
    if (!str_eq(json_str(json_get(s->pr, "state")), "open") || !json_str(json_get(s->pr, "headRef"))) return false;
    return screen_pulls_findings_unfixed(s) > 0;
}

void screen_pulls_layout_findings(PullScreen *s, Doc *doc, Col c) {
    int box = screen_pulls_col_box(doc, c);
    if (s->findings_error) { doc_notice(doc, c.ix, c.iw, s->findings_error); doc_space(doc, px(6)); }
    size_t fn = json_count(s->findings);
    for (size_t i = 0; i < fn; i++) {
        const Json *f = json_at(s->findings, i);
        if (i) screen_pulls_col_gap(doc, c);
        bool open = screen_pulls_finding_open(s, (int)i);
        const char *ft = json_str(json_get(f, "title"));
        int li = doc_label(doc, c.ix, c.iw, open ? 0xE70D : 0xE76C, ft ? ft : "Finding", FONT_CALLOUT, theme.text);
        doc_item(doc, li)->action = ACT_FINDING_TOGGLE; doc_item(doc, li)->arg = (intptr_t)i; doc_item(doc, li)->hand = true;
        Str meta; str_init(&meta);
        if (json_str(json_get(f, "severity"))) str_appendz(&meta, json_str(json_get(f, "severity")));
        bool fixed = json_bool_is(json_get(f, "fixed"), true);
        const char *decision = json_str(json_get(f, "decision"));
        if (fixed) str_appendf(&meta, "%sFixed", meta.len ? " \xC2\xB7 " : "");
        else if (decision) {
            const char *dt = decision; for (int k = 1; k < 4; k++) if (str_eq(decision, screen_pulls_decision_ids[k])) dt = decision_titles[k];
            str_appendf(&meta, "%s%s", meta.len ? " \xC2\xB7 " : "", dt);
        }
        if (str_eq(s->deciding, json_str(json_get(f, "key")))) str_appendz(&meta, " \xC2\xB7 saving\xE2\x80\xA6");
        if (meta.len) { doc_space(doc, px(2)); doc_text(doc, c.ix + px(22), c.iw - px(22), meta.data, FONT_CAPTION, fixed ? theme.success : str_eq(decision, "fix") ? theme.warning : theme.secondary, DT_SINGLELINE | DT_END_ELLIPSIS); }
        str_free(&meta);
        if (open) {
            const char *file = json_str(json_get(f, "file"));
            if (file) { double line; char *loc = json_num(json_get(f, "line"), &line) ? xstrfmt("%s:%d", file, (int)line) : xstrdup(file); doc_space(doc, px(4)); doc_text(doc, c.ix + px(22), c.iw - px(22), loc, FONT_MONO_SMALL, theme.text, DT_WORDBREAK); free(loc); }
            if (safe_web_url(json_str(json_get(f, "url")))) { doc_space(doc, px(4)); int ui = doc_text(doc, c.ix + px(22), c.iw - px(22), "Open finding on GitHub", FONT_CAPTION, theme.accent, DT_SINGLELINE); doc_item(doc, ui)->action = ACT_FINDING_URL; doc_item(doc, ui)->arg = (intptr_t)i; doc_item(doc, ui)->hand = true; }
            if (store_supports("finding_decision") && json_str(json_get(f, "key")) && !fixed) {
                int selected = 0;
                for (int k = 1; k < 4; k++) if (str_eq(decision, screen_pulls_decision_ids[k])) selected = k;
                doc_space(doc, px(8));
                doc_segments(doc, c.ix + px(22), c.iw - px(22), decision_titles, 4, selected, ACT_FINDING_DECISION, (intptr_t)(i * 8), s->deciding == NULL);
            }
        }
    }
    if (!fn && !s->findings_error) doc_text(doc, c.ix, c.iw, "No findings reported", FONT_CALLOUT, theme.secondary, DT_SINGLELINE);
    if (store_supports("finding_decision") && fn) { doc_space(doc, px(8)); doc_text(doc, c.ix, c.iw, "Decisions are saved on the dashboard and mirrored to the pull request\xE2\x80\x99s checklist on GitHub.", FONT_CAPTION, theme.secondary, DT_WORDBREAK); }
    if (screen_pulls_solve_findings_offered(s)) {
        size_t fixes = screen_pulls_findings_to_fix(s);
        doc_space(doc, px(12));
        char *label = s->busy ? xstrdup("Starting\xE2\x80\xA6") : fixes ? xstrfmt("Solve findings \xC2\xB7 %zu to fix", fixes) : xstrdup("Solve findings");
        ButtonSpec b = { 0, label, BUTTON_PROMINENT, ACT_SOLVE_FINDINGS, 0, !s->busy && !s->uncertain && !s->deciding };
        doc_button_row(doc, c.ix, c.iw, &b, 1);
        free(label);
        doc_space(doc, px(6));
        doc_text(doc, c.ix, c.iw, fixes ? "Starts a paid session that addresses the findings marked Fix, pushes the fixes and has them reviewed again."
                                        : "Starts a paid session that addresses the open findings, pushes the fixes and has them reviewed again. Mark what to fix first to narrow it down.",
                 FONT_CAPTION, theme.secondary, DT_WORDBREAK);
    }
    doc_box_end(doc, box, px(12));
}
