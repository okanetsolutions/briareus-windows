#include "screen_settings_internal.h"

static void layout_tabs(FormScreen *s, Doc *doc, int x, int w);
static void paint_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void paint_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void check(FormScreen *s, Doc *doc, int x, int w, int f);
static char *pick_text(FormScreen *s, int r, int part);
static void runtime_row(FormScreen *s, Doc *doc, int x, int w, int r);

static const struct { const char *title; wchar_t glyph; } TABS[T_COUNT] = {
    [T_PROJECT] = { "Project", 0xE8B7 }, [T_DATABASE] = { "Database", 0xE1D3 },
    [T_REVIEW] = { "Code review", 0xE721 }, [T_ORCHESTRATOR] = { "Orchestrator", 0xE716 }, [T_ENV] = { "Checkout .env", 0xE8D7 },
    [T_RUN] = { "Run", 0xE768 },
};

// MARK: Layout

/// Whether a tab holds a change not saved yet, for the dot after its title.
bool screen_settings_tab_changed(FormScreen *s, int t) {
    for (int f = 0; f < F_COUNT; f++) {
        if (screen_settings_field_tab(f) != t || !screen_settings_field_offered(s, f)) continue;
        if (f == F_BABYSIT) { if (project_babysit_changed(&s->babysit)) return true; continue; }
        if (!screen_settings_is_edit(f)) { if (s->bools[f] != json_bool_is(json_get(s->row, screen_settings_FIELDS[f].key), true)) return true; continue; }
        if (!s->edits[f]) continue;
        char *now = screen_settings_edit_text(s->edits[f]), *saved = screen_settings_field_text(s->row, f);
        bool differs = !str_eq(now, saved);
        free(now); free(saved);
        if (differs) return true;
    }
    for (int r = 0; r < R_COUNT; r++) {
        if (screen_settings_runtime_tab(r) != t) continue;
        RuntimePick saved = { 0 };
        screen_settings_row_pick(s, r, &saved);
        const RuntimePick *p = &s->picks[r];
        bool differs = p->provider_id != saved.provider_id
            || (p->provider_id && (!str_eq(p->model ? p->model : "", saved.model ? saved.model : "") || !str_eq(p->effort ? p->effort : "", saved.effort ? saved.effort : "")));
        screen_settings_pick_clear(&saved);
        if (differs) return true;
    }
    return false;
}

/// GitHub's `tabnav` over the form: one tab per section of the dashboard's form, a dot after any with unsaved changes.
static void layout_tabs(FormScreen *s, Doc *doc, int x, int w) {
    int h = px(42), tx = x, ty = doc->y;
    for (int t = 0; t < T_COUNT; t++) {
        bool changed = s->dirty && screen_settings_tab_changed(s, t);
        if (t == screen_settings_g_tab) s->tab_dot = changed;
        char *title = changed ? xstrfmt("%s \xE2\x80\xA2", TABS[t].title) : xstrdup(TABS[t].title);
        doc_tab(doc, &tx, &ty, x, x + w, h, TABS[t].glyph, title, NULL, t == screen_settings_g_tab, ACT_TAB, t);
        free(title);
    }
    doc->y = ty + h;
    doc_rule(doc, x, w);
    doc_space(doc, px(18));
}

static void paint_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    FormScreen *s = it->data;
    int f = (int)it->arg;
    bool on = screen_settings_field_enabled(s, f);
    COLORREF border = s->focused == f ? theme.accent_dim : theme.line;
    // A disabled edit paints the same fill, so only the border says the box is off.
    fill_round_rect(cv, rc, px(6), theme.raise, on ? border : blend(border, theme.canvas, 0.5));
}

/// A labelled box with its edit, the hint behind a help icon beside the label; advances.
void screen_settings_field(FormScreen *s, Doc *doc, int x, int w, int f) {
    if (!screen_settings_field_offered(s, f)) return;
    const FieldDef *d = &screen_settings_FIELDS[f];
    bool on = screen_settings_field_enabled(s, f);
    doc_field_label(doc, x, w, d->label, on ? theme.ink : theme.muted, d->hint);
    doc_space(doc, px(6));
    FontId fid = d->mono ? FONT_MONO : FONT_BODY;
    int fh = edit_line_height(fid);
    int h = screen_settings_is_multiline(f) ? screen_settings_shown_rows(s, f) * fh + px(16) : px(36);
    RECT box = { x, doc->y, x + w, doc->y + h };
    int i = doc_add(doc, &box, paint_box);
    Item *it = doc_item(doc, i);
    it->data = s; it->arg = f; it->action = ACT_FOCUS;
    RECT er = screen_settings_is_multiline(f) ? (RECT){ x + px(10), box.top + px(8), x + w - px(3), box.bottom - px(8) }
                              : (RECT){ x + px(10), box.top + (h - fh) / 2, x + w - px(10), box.top + (h - fh) / 2 + fh };
    s->rects[f] = er; s->laid[f] = true;
    if (s->edits[f]) EnableWindow(s->edits[f], on);
    doc->y = box.bottom;
    doc_space(doc, px(14));
}

static void paint_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    FormScreen *s = it->data;
    int f = (int)it->arg;
    bool hovered = doc_item_hovered(doc, it);
    // A pixel under the row's middle, so the box sits on the label's capitals rather than its line box.
    int size = px(15), top = rc->top + (rc->bottom - rc->top - size) / 2 + px(1);
    RECT b = { rc->left, top, rc->left + size, top + size };
    bool on = screen_settings_field_bool(s, f);
    fill_round_rect(cv, &b, px(3), on ? theme.accent : theme.field, on ? theme.accent : hovered ? theme.accent_dim : theme.line_strong);
    if (on) draw_check_mark(cv, &b, theme.on_accent);
    RECT t = { b.right + px(8), rc->top, rc->right, rc->bottom };
    draw_text(cv, screen_settings_FIELDS[f].label, &t, FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void check(FormScreen *s, Doc *doc, int x, int w, int f) {
    if (!screen_settings_field_offered(s, f)) return;
    int i = doc_custom(doc, x, w, px(26), paint_check, s, NULL, ACT_TOGGLE, f);
    doc_item(doc, i)->hand = true;
    doc_space(doc, px(10));
}

void screen_settings_select_free(void *p) { SelectData *d = p; free(d->text); free(d); }

void screen_settings_paint_select(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    SelectData *d = it->data;
    bool hovered = d->enabled && doc_item_hovered(doc, it);
    fill_round_rect(cv, rc, px(6), d->enabled ? theme.raise : blend(theme.raise, theme.canvas, 0.5), hovered ? theme.accent_dim : theme.line);
    RECT t = { rc->left + px(12), rc->top, rc->right - px(28), rc->bottom };
    draw_text(cv, d->text, &t, FONT_FOOTNOTE, d->enabled ? theme.ink : theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT c = { rc->right - px(26), rc->top, rc->right - px(10), rc->bottom };
    draw_glyph(cv, 0xE70D, &c, FONT_ICON_SMALL, d->enabled ? theme.ink : theme.muted);
}

static char *pick_text(FormScreen *s, int r, int part) {
    const RuntimePick *p = &s->picks[r];
    if (part == 0) {
        if (!p->provider_id) return xstrdup(screen_settings_RUNTIMES[r].none);
        const RuntimeProvider *pr = s->has_catalog ? runtime_catalog_provider(&s->catalog, p->provider_id) : NULL;
        return pr ? xstrdup(pr->label) : xstrfmt("Provider #%d%s", p->provider_id, s->has_catalog ? " (unavailable)" : "");
    }
    if (!p->provider_id) return xstrdup("\xE2\x80\x94");
    if (part == 1) {
        RuntimeChoice c = { p->provider_id, p->model, p->effort };
        const RuntimeModel *m = s->has_catalog ? runtime_catalog_model(&s->catalog, &c) : NULL;
        return xstrdup(m ? runtime_model_title(m) : !str_empty(p->model) ? p->model : "\xE2\x80\x94");
    }
    return xstrdup(!str_empty(p->effort) ? p->effort : "\xE2\x80\x94");
}

/// Provider, model and effort side by side, as the dashboard's `.runtime-row`; advances.
static void runtime_row(FormScreen *s, Doc *doc, int x, int w, int r) {
    if (!screen_settings_row_has(s, screen_settings_RUNTIMES[r].step ? "stepRuntimes" : screen_settings_RUNTIMES[r].provider_key)) return;
    int gap = px(14), cw = (w - 2 * gap) / 3, top = doc->y, h = px(36);
    static const char *const labels[] = { NULL, "Model", "Effort" };
    for (int part = 0; part < 3; part++) {
        int cx = x + part * (cw + gap), width = part == 2 ? w - 2 * (cw + gap) : cw;
        bool enabled = part == 0 || s->picks[r].provider_id != 0;
        RECT lr = { cx, top, cx + width, top + font_height(doc->cv, FONT_FOOTNOTE) };
        doc_text_at(doc, &lr, part ? labels[part] : screen_settings_RUNTIMES[r].provider_label, FONT_FOOTNOTE, enabled ? theme.ink : theme.muted, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        int by = lr.bottom + px(6);
        RECT box = { cx, by, cx + width, by + h };
        int i = doc_add(doc, &box, screen_settings_paint_select);
        Item *it = doc_item(doc, i);
        SelectData *d = xcalloc(1, sizeof *d); d->text = pick_text(s, r, part); d->enabled = enabled;
        it->data = d; it->free_data = screen_settings_select_free;
        if (enabled) { it->action = ACT_PICK; it->arg = r * 3 + part; it->hand = true; }
        doc->y = box.bottom;
    }
    doc_space(doc, px(14));
}

void screen_settings_note(Doc *doc, int x, int w, const char *text) {
    doc_text(doc, x, w, text, FONT_CAPTION, theme.muted, DT_LEFT | DT_WORDBREAK);
    doc_space(doc, px(10));
}

void screen_settings_form_layout(Screen *base, Doc *doc) {
    FormScreen *s = (FormScreen *)base;
    memset(s->laid, 0, sizeof s->laid);
    // The whole pane, as the pull request page uses it, rather than the dashboard's 720px column.
    int col = doc->width, x = 0;
    doc_space(doc, px(8));
    char *why = screen_settings_settings_unavailable();
    if (why) { doc_space(doc, px(10)); doc_text(doc, x, col, why, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK); free(why); doc_space(doc, px(12)); return; }
    layout_tabs(s, doc, x, col);
    if (s->error) { doc_notice_box(doc, x, col, s->error); doc_space(doc, px(16)); }
    switch (screen_settings_g_tab) {
    case T_PROJECT: {
        int gap = px(14), half = (col - gap) / 2, top = doc->y;
        screen_settings_field(s, doc, x, half, F_REPO);
        int left_bottom = doc->y;
        doc->y = top;
        screen_settings_field(s, doc, x + half + gap, col - half - gap, F_LABEL);
        if (doc->y < left_bottom) doc->y = left_bottom;
        check(s, doc, x, col, F_ENABLED);
        if (screen_settings_field_offered(s, F_ENABLED) && !s->bools[F_ENABLED])
            screen_settings_note(doc, x, col, "Inactive: no session can be started on it and it is left out of the project lists; its settings are kept for when it is switched back on.");
        check(s, doc, x, col, F_MAIL_TOOLS);
        if (screen_settings_field_offered(s, F_MAIL_TOOLS))
            screen_settings_note(doc, x, col, "Allows interactive sessions in this project to access the operator's connected mailboxes. Off by default.");
        doc_space(doc, px(4));
        screen_settings_field(s, doc, x, col, F_LOCAL_DIR);
        screen_settings_field(s, doc, x, col, F_BOARD);
        screen_settings_field(s, doc, x, col, F_SETUP);
        screen_settings_field(s, doc, x, col, F_PHP);
        screen_settings_note(doc, x, col, "This project's own prompt wording is edited under Prompts on the web dashboard; saving here keeps it as it is.");
        break;
    }
    case T_DATABASE:
        screen_settings_field(s, doc, x, col, F_DB_NAME); screen_settings_field(s, doc, x, col, F_DB_EXT);
        check(s, doc, x, col, F_DB_POOL); screen_settings_field(s, doc, x, col, F_DB_RESTORE);
        break;
    case T_REVIEW:
        screen_settings_field(s, doc, x, (col - px(28)) / 3, F_REVIEW_AUTHOR);
        runtime_row(s, doc, x, col, R_REVIEW);
        screen_settings_field(s, doc, x, col, F_PUBLISH);
        check(s, doc, x, col, F_AUTO_LOOP);
        if (screen_settings_field_offered(s, F_AUTO_LOOP) && s->bools[F_AUTO_LOOP])
            screen_settings_note(doc, x, col, "Every finding of a session's review-loop round, low severity and parked ones too, goes to \xE2\x9A\x99 Implement feedback on its own, and the fix is pushed and reviewed again until a round comes back clean and code-approved. The loop's round limit and repeated-findings check still stop one that cannot converge. Sessions still need their review loop switched on; a standalone \xE2\x8C\x95 Code review keeps its manual findings.");
        check(s, doc, x, col, F_BABYSIT);
        const char *babysit_note = project_babysit_note(&s->babysit);
        if (babysit_note) screen_settings_note(doc, x, col, babysit_note);
        // Each step runs as a turn of its own, on the code review's runtime unless it names one; a step switched off has none.
        check(s, doc, x, col, F_TEST_SHEET);
        if (s->bools[F_TEST_SHEET]) runtime_row(s, doc, x + px(23), col - px(23), R_TEST_SHEET);
        check(s, doc, x, col, F_TEST_RUN);
        if (s->bools[F_TEST_RUN]) runtime_row(s, doc, x + px(23), col - px(23), R_TEST_RUN);
        doc_space(doc, px(4));
        screen_settings_field(s, doc, x, col, F_QA_NOTES); screen_settings_field(s, doc, x, col, F_SHEET_STEPS); screen_settings_field(s, doc, x, col, F_FEEDBACK_STEPS);
        break;
    case T_ORCHESTRATOR:
        runtime_row(s, doc, x, col, R_WORKER);
        screen_settings_field(s, doc, x, col, F_BUDGET);
        screen_settings_note(doc, x, col, "The orchestrator's standing instructions for this project live under Prompts on the web dashboard, in the \xE2\x80\x9C" "Orchestrator instructions\xE2\x80\x9D template.");
        check(s, doc, x, col, F_IS_SELF);
        screen_settings_note(doc, x, col, "Tick it on the repository whose code is running right now, and on no other. An orchestrator on any project that finds a flaw in the tooling running it (a briefing, a worker tool, a loop) can then send a fix worker here, review loop armed, and merge its pull request once the loop approves and the checks are green. The running dashboard keeps its code until you redeploy.");
        break;
    case T_ENV: screen_settings_field(s, doc, x, col, F_ENV); break;
    case T_RUN: screen_settings_field(s, doc, x, col, F_RUN); screen_settings_field(s, doc, x, col, F_PROFILES); break;
    }
    doc_space(doc, px(40));
}

void screen_settings_form_header(Screen *base, HeaderInfo *info) {
    FormScreen *s = (FormScreen *)base;
    const char *label = json_str_nonempty(json_get(s->row, "label")), *repo = json_str_nonempty(json_get(s->row, "repo"));
    if (s->id) {
        snprintf(info->title, sizeof info->title, "%s", label ? label : repo ? repo : "Project");
        snprintf(info->subtitle, sizeof info->subtitle, "%s", repo ? repo : "");
    } else {
        snprintf(info->title, sizeof info->title, "New project");
        snprintf(info->subtitle, sizeof info->subtitle, "A project is a repository a session can be started against.");
    }
    if (!store_supports("settings_projects")) return;
    bool busy = s->req_save || s->req_delete;
    HeaderButton *b = &info->buttons[info->button_count++];
    snprintf(b->label, sizeof b->label, "%s", s->req_save ? "Saving\xE2\x80\xA6" : "Save");
    b->glyph = 0xE74E; b->action = ACT_SAVE; b->prominent = true; b->tip = "Save this project (Ctrl+S)";
    b->enabled = !busy && (s->dirty || !s->id) && store_supports(s->id ? "update_project" : "create_project");
    if (!s->id) return;
    HeaderButton *c = &info->buttons[info->button_count++];
    c->glyph = 0xE8C8; c->action = ACT_CLONE; c->enabled = !busy && store_supports("create_project"); c->tip = "Clone into a new project";
    HeaderButton *d = &info->buttons[info->button_count++];
    d->glyph = 0xE74D; d->action = ACT_DELETE; d->destructive = true; d->enabled = !busy && store_supports("delete_project"); d->tip = "Delete this project";
}
