#include "screen_ci_internal.h"
#include "stat_components.h"

static void filters(CIScreen *s, Doc *doc, int w) {
    char *repo = xstrfmt("Project: %s", s->repo && *s->repo ? s->repo : "All"), *branch = xstrfmt("Branch: %s", s->branch && *s->branch ? s->branch : "All");
    ButtonSpec buttons[] = {
        {0, "7 days", s->days == 7 ? BUTTON_PROMINENT : BUTTON_BORDERED, CI_DAYS, 7, true},
        {0, "30 days", s->days == 30 ? BUTTON_PROMINENT : BUTTON_BORDERED, CI_DAYS, 30, true},
        {0, "90 days", s->days == 90 ? BUTTON_PROMINENT : BUTTON_BORDERED, CI_DAYS, 90, true},
        {0, "Checks", !s->statuses ? BUTTON_PROMINENT : BUTTON_BORDERED, CI_KIND, 0, true},
        {0, "Legacy statuses", s->statuses ? BUTTON_PROMINENT : BUTTON_BORDERED, CI_KIND, 1, true},
        {0, repo, BUTTON_BORDERED, CI_REPO, 0, true}, {0, branch, BUTTON_BORDERED, CI_BRANCH, 0, true},
    };
    doc_button_row(doc, 0, w, buttons, sizeof buttons / sizeof *buttons); free(repo); free(branch);
}
static void metrics(Doc *doc, const Json *m, int x, int w) {
    char *total = ci_metric_text(m, "total"), *passed = ci_metric_text(m, "succeeded"), *failed = ci_metric_text(m, "failed");
    char *rate = ci_metric_text(m, "successRate"), *duration = ci_metric_text(m, "avgDurationMs");
    char *text = xstrfmt("%s total · %s passed · %s failed · %s success · %s avg", total, passed, failed, rate, duration);
    doc_text(doc, x, w, text, FONT_FOOTNOTE, theme.secondary, DT_WORDBREAK);
    free(text); free(total); free(passed); free(failed); free(rate); free(duration);
}
static void groups(Doc *doc, const Json *rows, const char *key, const char *title, int w) {
    doc_section(doc, 0, w, title);
    if (!json_count(rows)) doc_text(doc, 0, w, "No observations in this window.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    for (size_t i = 0; i < json_count(rows); i++) {
        const Json *row = json_at(rows, i);
        int box = doc_box_begin(doc, 0, w, px(10), theme.raise, theme.line, px(10));
        doc_text(doc, px(10), w - px(20), json_str(json_get(row, key)), FONT_BODY, theme.text, DT_WORDBREAK);
        const char *app = json_str_nonempty(json_get(row, "app"));
        if (app) doc_text(doc, px(10), w - px(20), app, FONT_CAPTION, theme.muted, DT_WORDBREAK);
        metrics(doc, row, px(10), w - px(20)); doc_box_end(doc, box, px(10)); doc_space(doc, px(4));
    }
}
static void figures(CIScreen *s, Doc *doc, int w) {
    const Json *totals = json_get(s->stats, "totals");
    const char *keys[] = {"total", "successRate", "failed", "avgDurationMs", "pending"};
    const char *labels[] = {"Total", "Success rate", "Failed", "Average duration", "Pending"};
    StatTile tiles[5] = {0};
    for (int i = 0; i < 5; i++) { tiles[i].label = labels[i]; tiles[i].value = ci_metric_text(totals, keys[i]); }
    doc_stat_tiles(doc, 0, w, tiles, 5); stats_tiles_free(tiles, 5);
    groups(doc, json_get(s->stats, "daily"), "day", "Daily observations · UTC", w);
    groups(doc, json_get(s->stats, "repositories"), "repo", "Repositories", w);
    groups(doc, json_get(s->stats, "checks"), "name", "Checks", w);
}
void ci_layout(Screen *base, Doc *doc) {
    CIScreen *s = (CIScreen *)base; int w = doc->width;
    s->action_first = s->action_next;
    doc_space(doc, px(12));
    if (!ci_current(s)) { doc_notice(doc, 0, w, "Reopen CI/CD on a connection with CI history access."); return; }
    filters(s, doc, w);
    doc_text(doc, 0, w, "Observed GitHub checks only, not a full backfill. Refresh starts a new window; background reads retain this window and its run pages. Durations are execution time, not billed minutes.", FONT_CAPTION, theme.muted, DT_WORDBREAK);
    for (int i = 0; i < 2; i++) {
        if (s->errors[i]) doc_notice(doc, 0, w, s->errors[i]);
        if (s->retry[i] > ci_now(s)) {
            char *at = stats_time((StatNumber){s->retry[i] * 1000, true});
            char *text = xstrfmt("%s reads paused until %s", i ? "Run" : "Summary", at);
            doc_text(doc, 0, w, text, FONT_CAPTION, theme.muted, DT_WORDBREAK); free(at); free(text);
        }
    }
    if (s->reads[0]) doc_loading(doc, 0, w, "Loading CI figures…");
    if (s->stats) figures(s, doc, w);
    doc_section(doc, 0, w, "Recent runs");
    if (!store_supports("ci_runs")) { doc_notice(doc, 0, w, "This connection does not offer run history."); return; }
    if (s->reads[1]) doc_loading(doc, 0, w, "Loading runs…");
    if (s->runs && !json_count(s->runs)) doc_text(doc, 0, w, "No runs observed in this window.", FONT_BODY, theme.muted, DT_WORDBREAK);
    s->action_next += (intptr_t)json_count(s->runs);
    for (size_t i = 0; i < json_count(s->runs); i++) {
        const Json *r = json_at(s->runs, i); char *title = xstrfmt("%s · %s", ci_run_state(r), json_str(json_get(r, "name")));
        char *meta = ci_run_meta(r);
        int box = doc_box_begin(doc, 0, w, px(10), theme.raise, theme.line, px(10));
        doc_text(doc, px(10), w - px(20), title, FONT_BODY, theme.text, DT_WORDBREAK);
        doc_text(doc, px(10), w - px(20), meta, FONT_CAPTION, theme.secondary, DT_WORDBREAK);
        if (json_str_nonempty(json_get(r, "url"))) doc_button(doc, px(10), 0, "Open on GitHub", BUTTON_BORDERED, CI_OPEN, s->action_first + (intptr_t)i, true);
        doc_box_end(doc, box, px(10)); doc_space(doc, px(6)); free(title); free(meta);
    }
    if (s->truncated || s->capped) doc_notice(doc, 0, w, "More observations exist. Narrow the window, project or branch to see them.");
    if (s->offset >= 0) doc_button(doc, 0, 0, "Load more", BUTTON_BORDERED, CI_MORE, 0, !s->reads[0] && !s->reads[1] && s->retry[1] <= ci_now(s));
}
