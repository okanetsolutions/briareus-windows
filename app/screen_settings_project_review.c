#include "screen_settings_internal.h"

// The project form's Code review tab: the PR author beside the review round limit, the review rules (the security
// pass's policy, publishing the review outcome to GitHub, the review benchmark) and the errands' runtimes and steps.
// The security review is a K_CHOICE field, the form's only one; a server whose projects lack a key neither shows nor
// receives it (screen_settings_row_has).

static const char *choice_saved(const FormScreen *s, int f) {
    const char *v = json_str(json_get(s->row, screen_settings_FIELDS[f].key));
    return v ? v : "";
}
void screen_settings_choices_fill(FormScreen *s) {
    for (int f = 0; f < F_COUNT; f++) if (screen_settings_FIELDS[f].kind == K_CHOICE) set_string(&s->choices[f], choice_saved(s, f));
}
void screen_settings_choices_free(FormScreen *s) {
    for (int f = 0; f < F_COUNT; f++) { free(s->choices[f]); s->choices[f] = NULL; }
}
bool screen_settings_choice_changed(FormScreen *s, int f) {
    return !str_eq(s->choices[f] ? s->choices[f] : "", choice_saved(s, f));
}
bool screen_settings_choose(FormScreen *s, int f, size_t index) {
    if (f < 0 || f >= F_COUNT || screen_settings_FIELDS[f].kind != K_CHOICE || !screen_settings_field_offered(s, f)) return false;
    const FieldDef *d = &screen_settings_FIELDS[f];
    if (index >= d->choice_count) return false;
    set_string(&s->choices[f], d->choices[index].value);
    return true;
}
static int choice_index(const FormScreen *s, int f) {
    const FieldDef *d = &screen_settings_FIELDS[f];
    for (size_t i = 0; i < d->choice_count; i++) if (str_eq(s->choices[f], d->choices[i].value)) return (int)i;
    return -1;
}
// Left empty (a row from before the setting), the server keeps what it has; a value the field does not list is refused.
static char *choice_value(FormScreen *s, int f, Json *body) {
    const FieldDef *d = &screen_settings_FIELDS[f];
    const char *v = s->choices[f] ? s->choices[f] : "";
    if (!*v) { json_object_remove(body, d->key); return NULL; }
    if (choice_index(s, f) >= 0) { json_set_str(body, d->key, v); return NULL; }
    Str titles; str_init(&titles);
    for (size_t i = 0; i < d->choice_count; i++) { if (i) str_appendz(&titles, ", "); str_appendz(&titles, d->choices[i].title); }
    char *why = xstrfmt("Pick one of %s for %s.", titles.data ? titles.data : "", d->label);
    str_free(&titles);
    return why;
}
// A whole number from 0 (no limit) to 20; left empty, the server's default.
static char *round_limit_value(FormScreen *s, Json *body) {
    if (!s->edits[F_ROUND_LIMIT]) return NULL;
    char *text = screen_settings_edit_text(s->edits[F_ROUND_LIMIT]), *t = str_trim(text), *end = NULL;
    free(text);
    long n = *t ? strtol(t, &end, 10) : 0;
    char *why = NULL;
    if (!*t) json_object_set(body, "reviewRoundLimit", json_null());
    else if (*end || n < 0 || n > 20) why = xstrfmt("%s must be a whole number from 1 to 20, or 0 for no limit.", screen_settings_FIELDS[F_ROUND_LIMIT].label);
    else json_set_num(body, "reviewRoundLimit", (double)n);
    free(t);
    return why;
}
char *screen_settings_review_value(FormScreen *s, int f, Json *body) {
    return f == F_ROUND_LIMIT ? round_limit_value(s, body) : choice_value(s, f, body);
}

// MARK: Layout

static const char *rule_note(int f, bool on) {
    if (f == F_ENFORCE_OUTCOME) return on
        ? "Each pull request's review outcome is written to its head as briareus/code-review, and its approvals as briareus/approvals, which branch protection can require. The server's GitHub token must be allowed to write commit statuses."
        : "The outcome is still computed and shown here; nothing is written to GitHub.";
    if (f == F_BENCHMARK && on) return "A benchmark run opens pull requests and branches in this repository and closes them again: keep it for a repository nobody works in.";
    return NULL;
}
static void rule(FormScreen *s, Doc *doc, int x, int w, int f) {
    if (!screen_settings_field_offered(s, f)) return;
    screen_settings_check(s, doc, x, w, f);
    const char *note = rule_note(f, s->bools[f]);
    if (note) screen_settings_note(doc, x, w, note);
}
static void choice(FormScreen *s, Doc *doc, int x, int w, int f) {
    if (!screen_settings_field_offered(s, f)) return;
    const FieldDef *d = &screen_settings_FIELDS[f];
    doc_field_label(doc, x, w, d->label, theme.ink, d->hint);
    doc_space(doc, px(6));
    const char *titles[CHOICE_STRIDE] = {0};
    size_t n = d->choice_count < CHOICE_STRIDE ? d->choice_count : CHOICE_STRIDE;
    for (size_t i = 0; i < n; i++) titles[i] = d->choices[i].title;
    doc_segments(doc, x, w, titles, n, choice_index(s, f), ACT_CHOOSE, (intptr_t)f * CHOICE_STRIDE, true);
    doc_space(doc, px(14));
}
void screen_settings_review_layout(FormScreen *s, Doc *doc, int x, int col) {
    int gap = px(14), third = (col - 2 * gap) / 3, top = doc->y;
    screen_settings_field(s, doc, x, third, F_REVIEW_AUTHOR);
    int bottom = doc->y;
    doc->y = top;
    screen_settings_field(s, doc, screen_settings_field_offered(s, F_REVIEW_AUTHOR) ? x + third + gap : x, third, F_ROUND_LIMIT);
    if (doc->y < bottom) doc->y = bottom;
    choice(s, doc, x, col, F_SECURITY_REVIEW);
    rule(s, doc, x, col, F_ENFORCE_OUTCOME);
    rule(s, doc, x, col, F_BENCHMARK);
    screen_settings_runtime_row(s, doc, x, col, R_REVIEW);
    screen_settings_field(s, doc, x, col, F_PUBLISH);
    screen_settings_check(s, doc, x, col, F_AUTO_LOOP);
    if (screen_settings_field_offered(s, F_AUTO_LOOP) && s->bools[F_AUTO_LOOP])
        screen_settings_note(doc, x, col, "Every finding of a session's review-loop round, low severity and parked ones too, goes to \xE2\x9A\x99 Implement feedback on its own, and the fix is pushed and reviewed again until a round comes back clean and code-approved. The loop's round limit and repeated-findings check still stop one that cannot converge. Sessions still need their review loop switched on; a standalone \xE2\x8C\x95 Code review keeps its manual findings.");
    // Each step runs as a turn of its own, on the code review's runtime unless it names one; a step switched off has none.
    screen_settings_check(s, doc, x, col, F_TEST_SHEET);
    if (s->bools[F_TEST_SHEET]) screen_settings_runtime_row(s, doc, x + px(23), col - px(23), R_TEST_SHEET);
    screen_settings_check(s, doc, x, col, F_TEST_RUN);
    if (s->bools[F_TEST_RUN]) screen_settings_runtime_row(s, doc, x + px(23), col - px(23), R_TEST_RUN);
    doc_space(doc, px(4));
    screen_settings_field(s, doc, x, col, F_QA_NOTES);
    screen_settings_field(s, doc, x, col, F_SHEET_STEPS);
    screen_settings_field(s, doc, x, col, F_FEEDBACK_STEPS);
}
