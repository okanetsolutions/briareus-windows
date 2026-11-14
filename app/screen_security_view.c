#include "screen_security_internal.h"

// The Security screen's page: how far the scan got, the pull requests counted per result, then each pull request worst
// first with its result, why, and how its findings stand. One whose project asks for no security pass is only counted.

static const ReviewSecurity *security_of(const SecurityPull *p) { return p->has_outcome && p->outcome.security.present ? &p->outcome.security : NULL; }
// Worst first; within a rank, by repository and number.
static const SecurityScreen *sorting;
static int by_rank(const void *a, const void *b) {
    const SecurityPull *x = &sorting->pulls[*(const size_t *)a], *y = &sorting->pulls[*(const size_t *)b];
    int rx = (int)review_security_rank(security_of(x)), ry = (int)review_security_rank(security_of(y));
    if (rx != ry) return rx - ry;
    int repo = strcmp(x->project.repo, y->project.repo);
    return repo ? repo : x->number - y->number;
}
static void progress(SecurityScreen *s, Doc *doc, int w) {
    if (s->error) { doc_notice(doc, 0, w, s->error); return; }
    if (s->skipped) {
        char *text = xstrfmt("The pull requests of %s could not be read. Refresh to try again.", s->skipped);
        doc_notice(doc, 0, w, text); free(text);
    }
    if (s->capped) {
        char *text = xstrfmt("Showing the first %d open pull requests.", SECURITY_PULL_LIMIT);
        doc_text(doc, 0, w, text, FONT_CAPTION, theme.muted, DT_WORDBREAK); free(text);
    }
    if (security_done(s)) return;
    char *text = s->next_project < s->project_count || !s->project_count
        ? xstrdup("Listing open pull requests\xE2\x80\xA6")
        : xstrfmt("Reading the security review of %u of %u pull requests\xE2\x80\xA6", (unsigned)security_read_count(s) + 1, (unsigned)s->pull_count);
    doc_loading(doc, 0, w, text); free(text);
}
static void row(SecurityScreen *s, Doc *doc, int w, size_t index) {
    const SecurityPull *p = &s->pulls[index];
    const ReviewSecurity *sec = security_of(p);
    int pad = px(12), box = doc_box_begin(doc, 0, w, pad, theme.raise, theme.line, px(10));
    char *title = xstrfmt("%s #%d \xC2\xB7 %s", p->project.repo, p->number, p->title);
    doc_text(doc, pad, w - 2 * pad, title, FONT_BODY, theme.ink, DT_WORDBREAK); free(title);
    if (!p->read) doc_text(doc, pad, w - 2 * pad, "Not read yet", FONT_CAPTION, theme.muted, DT_WORDBREAK);
    else if (!sec) doc_text(doc, pad, w - 2 * pad, p->error ? p->error : "No review outcome from this server.", FONT_CAPTION, theme.muted, DT_WORDBREAK);
    else {
        char *line = review_security_line(sec), *findings = review_security_findings_line(sec), *risk = review_security_risk_note(sec);
        doc_text(doc, pad, w - 2 * pad, line, FONT_FOOTNOTE_SEMIBOLD, review_tone_color(review_security_tone(sec)), DT_WORDBREAK);
        if (findings) doc_text(doc, pad, w - 2 * pad, findings, FONT_FOOTNOTE, theme.ink, DT_WORDBREAK);
        if (risk) doc_text(doc, pad, w - 2 * pad, risk, FONT_CAPTION, theme.muted, DT_WORDBREAK);
        // The first reason that holds it back, in the server's words.
        for (size_t i = 0; i < sec->reasons.count; i++) {
            if (review_reason_is_note(&sec->reasons.items[i])) continue;
            doc_text(doc, pad, w - 2 * pad, sec->reasons.items[i].message, FONT_CAPTION, review_tone_color(review_reason_tone(&sec->reasons.items[i])), DT_WORDBREAK);
            break;
        }
        free(line); free(findings); free(risk);
    }
    doc_box_end(doc, box, pad);
    doc_box_action(doc, box, SECURITY_OPEN, (intptr_t)((s->generation & 0xFFFF) << 16) | (intptr_t)index);
    doc_space(doc, px(8));
}
void security_layout(Screen *base, Doc *doc) {
    SecurityScreen *s = (SecurityScreen *)base;
    int w = doc->width;
    doc_space(doc, px(12));
    if (!security_current(s)) { doc_notice(doc, 0, w, "Reopen Security on a connection whose server lists pull requests and their code review rounds."); return; }
    progress(s, doc, w);
    size_t counts[SECURITY_RANKS] = {0}, *order = xcalloc(s->pull_count + 1, sizeof *order), shown = 0;
    for (size_t i = 0; i < s->pull_count; i++) {
        if (s->pulls[i].read) counts[review_security_rank(security_of(&s->pulls[i]))]++;
        order[i] = i;
    }
    sorting = s; qsort(order, s->pull_count, sizeof *order, by_rank); sorting = NULL;
    if (security_read_count(s)) {
        char *summary = review_security_summary(counts);
        doc_text(doc, 0, w, summary, FONT_SUBHEADLINE_SEMIBOLD, theme.ink, DT_WORDBREAK); free(summary);
        doc_space(doc, px(12));
    } else if (security_done(s) && !s->skipped) doc_empty_state(doc, 0, w, 0xE73E, "No open pull requests", "Your projects have no open pull request to review.");
    for (size_t i = 0; i < s->pull_count; i++) {
        const SecurityPull *p = &s->pulls[order[i]];
        if (p->read && review_security_rank(security_of(p)) == SECURITY_RANK_OFF && !p->error) continue;
        row(s, doc, w, order[i]); shown++;
    }
    if (security_done(s) && s->pull_count && !shown) doc_text(doc, 0, w, "None of these projects asks for a security review. Turn it on in a project's settings, Code review tab.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    free(order);
}
