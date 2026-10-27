#include "screen_pulls_internal.h"

static bool catalog_lists(const Json *catalog, const char *id);

static const char *const decision_titles[] = { "Undecided", "Fix", "Optional", "Dismiss" };

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
