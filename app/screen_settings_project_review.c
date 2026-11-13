#include "screen_settings_internal.h"

static const FieldChoice *choice_of(int f, const char *value);
static void choice_select(FormScreen *s, Doc *doc, int x, int w, int f);
static void check_with_note(FormScreen *s, Doc *doc, int x, int w, int f, const char *note);

// The review rules on the Code review tab: the security pass's policy, a K_CHOICE picked from a menu, publishing the review
// outcome to GitHub, and setting the repository aside for the review benchmark, each switch with its note. The round limit
// is a box beside the PR author.

static const FieldChoice *choice_of(int f, const char *value) {
    for (const FieldChoice *c = screen_settings_FIELDS[f].choices; c && c->value; c++) if (str_eq(c->value, value)) return c;
    return NULL;
}

/// What the picker says for `value`: its title, or the value itself for one the field does not list.
const char *screen_settings_choice_title(int f, const char *value) {
    const FieldChoice *c = choice_of(f, value);
    return c ? c->title : value ? value : "";
}

bool screen_settings_choice_changed(FormScreen *s, int f) {
    char *saved = screen_settings_field_text(s->row, f);
    bool differs = !str_eq(s->choices[f] ? s->choices[f] : "", saved);
    free(saved);
    return differs;
}

/// Puts the field's value into `body`. Left empty (a row from before the setting) the key is left out, so the server keeps
/// what it has; false with `*why` for a value the field does not list.
bool screen_settings_choice_into(FormScreen *s, int f, Json *body, char **why) {
    const char *value = s->choices[f], *key = screen_settings_FIELDS[f].key;
    if (str_empty(value)) { json_object_remove(body, key); return true; }
    if (choice_of(f, value)) { json_set_str(body, key, value); return true; }
    Str titles; str_init(&titles);
    for (const FieldChoice *c = screen_settings_FIELDS[f].choices; c && c->value; c++) {
        if (titles.len) str_appendz(&titles, ", ");
        str_appendz(&titles, c->title);
    }
    *why = xstrfmt("Pick one of %s for %s.", titles.data ? titles.data : "", screen_settings_FIELDS[f].label);
    str_free(&titles);
    return false;
}

void screen_settings_choose(FormScreen *s, int f, POINT pt) {
    const FieldChoice *choices = screen_settings_FIELDS[f].choices;
    HMENU menu = CreatePopupMenu();
    int n = 0;
    for (; choices && choices[n].value; n++) screen_settings_append_item(menu, (UINT)n + 1, choices[n].title, str_eq(choices[n].value, s->choices[f]), true);
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(s->base.pane), NULL);
    DestroyMenu(menu);
    if (chosen < 1 || chosen > n || str_eq(choices[chosen - 1].value, s->choices[f])) return;
    set_string(&s->choices[f], choices[chosen - 1].value);
    screen_settings_form_changed(s);
    pane_relayout(s->base.pane);
}

/// A labelled picker, the hint behind a help icon beside the label, as wide as one of three columns; advances.
static void choice_select(FormScreen *s, Doc *doc, int x, int w, int f) {
    if (!screen_settings_field_offered(s, f)) return;
    const FieldDef *d = &screen_settings_FIELDS[f];
    int cw = (w - px(28)) / 3;
    doc_field_label(doc, x, cw, d->label, theme.ink, d->hint);
    doc_space(doc, px(6));
    RECT box = { x, doc->y, x + cw, doc->y + px(36) };
    int i = doc_add(doc, &box, screen_settings_paint_select);
    Item *it = doc_item(doc, i);
    SelectData *data = xcalloc(1, sizeof *data);
    data->text = xstrdup(screen_settings_choice_title(f, s->choices[f])); data->enabled = true;
    it->data = data; it->free_data = screen_settings_select_free;
    it->action = ACT_CHOOSE; it->arg = f; it->hand = true;
    doc->y = box.bottom;
    doc_space(doc, px(14));
}

static void check_with_note(FormScreen *s, Doc *doc, int x, int w, int f, const char *note) {
    if (!screen_settings_field_offered(s, f)) return;
    screen_settings_check(s, doc, x, w, f);
    if (note) screen_settings_note(doc, x, w, note);
}

void screen_settings_review_rules(FormScreen *s, Doc *doc, int x, int w) {
    choice_select(s, doc, x, w, F_SECURITY);
    check_with_note(s, doc, x, w, F_ENFORCE_OUTCOME, s->bools[F_ENFORCE_OUTCOME]
        ? "Each pull request's review outcome is written to its head as briareus/code-review, and its approvals as briareus/approvals, which branch protection can require. The server's GitHub token must be allowed to write commit statuses."
        : "The outcome is still computed and shown here; nothing is written to GitHub.");
    check_with_note(s, doc, x, w, F_BENCHMARK, s->bools[F_BENCHMARK]
        ? "A benchmark run opens pull requests and branches in this repository and closes them again: keep it for a repository nobody works in."
        : NULL);
}
