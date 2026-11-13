#include "code_reviews.h"
#include "str.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int64_t code_review_id(const Json *record) {
    double n;
    if (!json_num(json_get(record, "id"), &n) || n < 1 || n > 9007199254740991.0 || floor(n) != n) return 0;
    return (int64_t)n;
}
static bool finding_valid(const Json *f) {
    const char *severity = json_str(json_get(f, "severity"));
    if (!code_review_id(f) || !json_str(json_get(f, "title"))) return false;
    return str_eq(severity, "critical") || str_eq(severity, "high") || str_eq(severity, "medium") || str_eq(severity, "low");
}
static bool review_valid(const Json *r) {
    const Json *findings = json_get(r, "findings");
    const char *status = json_str(json_get(r, "status"));
    if (!code_review_id(r) || findings->type != JSON_ARRAY) return false;
    if (!str_eq(status, "running") && !str_eq(status, "completed") && !str_eq(status, "failed")) return false;
    for (size_t j = 0; j < json_count(findings); j++) {
        const Json *f = json_at(findings, j);
        if (!finding_valid(f)) return false;
        for (size_t k = 0; k < j; k++) if (code_review_id(json_at(findings, k)) == code_review_id(f)) return false;
    }
    return true;
}
Json *code_reviews_parse(const Json *response) {
    const Json *reviews = json_get(response, "codeReviews");
    if (reviews->type != JSON_ARRAY || json_count(reviews) > 20) return NULL;
    for (size_t i = 0; i < json_count(reviews); i++) {
        const Json *r = json_at(reviews, i);
        if (!review_valid(r)) return NULL;
        for (size_t j = 0; j < i; j++) if (code_review_id(json_at(reviews, j)) == code_review_id(r)) return NULL;
    }
    return json_clone(reviews);
}
const Json *code_review_find(const Json *reviews, int64_t id) {
    for (size_t i = 0; i < json_count(reviews); i++) if (code_review_id(json_at(reviews, i)) == id) return json_at(reviews, i);
    return NULL;
}
const Json *code_review_finding(const Json *review, int64_t id) { return code_review_find(json_get(review, "findings"), id); }
size_t code_reviews_findings(const Json *reviews) {
    size_t n = 0;
    for (size_t i = 0; i < json_count(reviews); i++) n += json_count(json_get(json_at(reviews, i), "findings"));
    return n;
}
bool code_reviews_running(const Json *reviews) {
    for (size_t i = 0; i < json_count(reviews); i++) if (str_eq(json_str(json_get(json_at(reviews, i), "status")), "running")) return true;
    return false;
}
const char *code_review_head(const Json *review, const char *current_head) {
    const char *head = json_str(json_get(review, "headSha"));
    if (str_empty(head) || str_empty(current_head)) return "head unknown";
    return str_eq(head, current_head) ? "current head" : "older head";
}
const char *code_review_verdict(const Json *review, const Json *finding) {
    const char *v = json_str(json_get(finding, "shouldFix"));
    if (!str_empty(v)) return v;
    return str_eq(json_str(json_get(review, "status")), "running") ? "pending" : "no verdict";
}
const char *code_review_verification(const Json *review, const Json *finding) {
    const char *v = json_str(json_get(finding, "verification"));
    if (!str_empty(v)) return v;
    return str_eq(json_str(json_get(review, "status")), "running") ? "pending" : "not verified";
}
char *code_review_location(const Json *finding) {
    const char *path = json_str(json_get(finding, "filePath"));
    double start, end;
    if (str_empty(path)) return xstrdup("No location");
    if (!json_num(json_get(finding, "startLine"), &start)) return xstrdup(path);
    if (json_num(json_get(finding, "endLine"), &end) && end != start) return xstrfmt("%s:%.0f-%.0f", path, start, end);
    return xstrfmt("%s:%.0f", path, start);
}
static char *number(const Json *review, const char *field) {
    double n;
    return json_num(json_get(review, field), &n) ? xstrfmt("%.0f", n) : xstrdup("unknown");
}
char *code_review_usage(const Json *review) {
    double cost;
    char *in = number(review, "inputTokens"), *out = number(review, "outputTokens");
    bool has_cost = json_num(json_get(review, "costUsd"), &cost);
    char *price = has_cost ? xstrfmt("$%.4f%s", cost, json_bool_is(json_get(review, "costEstimated"), true) ? " estimated" : "") : xstrdup("Cost unknown");
    int unpriced = json_int_or(json_get(review, "unpricedTurns"), 0);
    char *text = xstrfmt("%s in / %s out; %s; %d unpriced turns%s", in, out, price, unpriced, unpriced && has_cost ? " (partial cost)" : "");
    free(in); free(out); free(price); return text;
}
typedef struct { size_t critical, high, medium, low, pending, no_verdict, total; } Counts;
static void count_findings(Counts *c, const Json *review) {
    const Json *findings = json_get(review, "findings");
    c->total += json_count(findings);
    for (size_t i = 0; i < json_count(findings); i++) {
        const Json *f = json_at(findings, i); const char *s = json_str(json_get(f, "severity"));
        if (str_eq(s, "critical")) c->critical++;
        if (str_eq(s, "high")) c->high++;
        if (str_eq(s, "medium")) c->medium++;
        if (str_eq(s, "low")) c->low++;
        if (str_empty(json_str(json_get(f, "shouldFix")))) {
            if (str_eq(json_str(json_get(review, "status")), "running")) c->pending++;
            else c->no_verdict++;
        }
    }
}
static char *counts_text(const Counts *c) {
    return xstrfmt("%zu finding%s: %zu critical, %zu high, %zu medium, %zu low; %zu pending verdict%s; %zu without verdict",
        c->total, c->total == 1 ? "" : "s", c->critical, c->high, c->medium, c->low, c->pending, c->pending == 1 ? "" : "s", c->no_verdict);
}
char *code_review_counts(const Json *review) {
    Counts c = {0}; count_findings(&c, review); return counts_text(&c);
}
char *code_reviews_counts(const Json *reviews) {
    Counts c = {0};
    for (size_t i = 0; i < json_count(reviews); i++) count_findings(&c, json_at(reviews, i));
    return counts_text(&c);
}

static CodeReviewAmount amount(const Json *review, const char *name) {
    double n;
    if (!json_num(json_get(review, name), &n) || !isfinite(n) || n < 0) return (CodeReviewAmount){0, 0, 1};
    return (CodeReviewAmount){n, 1, 0};
}
CodeReviewTotals code_review_totals(const Json *review) {
    CodeReviewTotals t = {0};
    t.reviews = 1; t.findings = json_count(json_get(review, "findings"));
    t.input = amount(review, "inputTokens"); t.output = amount(review, "outputTokens"); t.cost = amount(review, "costUsd");
    t.estimated = t.cost.known && json_bool_is(json_get(review, "costEstimated"), true);
    int unpriced = json_int_or(json_get(review, "unpricedTurns"), 0);
    t.unpriced_turns = unpriced > 0 ? (size_t)unpriced : 0;
    return t;
}
static void add_amount(CodeReviewAmount *to, CodeReviewAmount from) {
    to->value += from.value; to->known += from.known; to->unknown += from.unknown;
}
CodeReviewTotals code_reviews_totals(const Json *reviews) {
    CodeReviewTotals t = {0};
    for (size_t i = 0; i < json_count(reviews); i++) {
        CodeReviewTotals r = code_review_totals(json_at(reviews, i));
        t.reviews++; t.findings += r.findings; t.unpriced_turns += r.unpriced_turns;
        add_amount(&t.input, r.input); add_amount(&t.output, r.output); add_amount(&t.cost, r.cost);
        t.estimated = t.estimated || r.estimated;
    }
    return t;
}
char *code_review_counts_compact(const Json *review) {
    Counts c = {0}; count_findings(&c, review);
    Str text; str_init(&text); str_appendf(&text, "%zu finding%s", c.total, c.total == 1 ? "" : "s");
    const size_t values[] = {c.critical, c.high, c.medium, c.low, c.pending, c.no_verdict};
    const char *labels[] = {"critical", "high", "medium", "low", "pending", "no verdict"};
    bool first = true;
    for (size_t i = 0; i < 4; i++) if (values[i]) {
        str_appendf(&text, "%s%zu %s", first ? "\n" : ", ", values[i], labels[i]); first = false;
    }
    for (size_t i = 4; i < 6; i++) if (values[i]) str_appendf(&text, "\n%zu %s", values[i], labels[i]);
    return str_detach(&text);
}

// MARK: - Handling and verdict edits

const char *code_review_handling(const Json *review) {
    const char *h = json_str(json_get(review, "handlingStatus"));
    return str_eq(h, "handled") || str_eq(h, "pending") ? h : NULL;
}
CodeReviewHandlingAction code_review_handling_action(const Json *review) {
    const char *h = code_review_handling(review);
    if (!h || !code_review_can_edit_findings(review)) return CODE_REVIEW_HANDLING_NONE;
    return str_eq(h, "handled") ? CODE_REVIEW_REOPEN : CODE_REVIEW_MARK_HANDLED;
}
const char *code_review_handling_title(CodeReviewHandlingAction action) {
    switch (action) {
    case CODE_REVIEW_MARK_HANDLED: return "Mark handled";
    case CODE_REVIEW_REOPEN: return "Reopen";
    default: return "";
    }
}
Json *code_review_handling_args(const char *repo, int number, int64_t review_id, CodeReviewHandlingAction action) {
    Json *a = json_object();
    json_set_str(a, "repo", repo); json_set_num(a, "pr", number); json_set_num(a, "reviewId", (double)review_id);
    json_set_str(a, "handlingStatus", action == CODE_REVIEW_MARK_HANDLED ? "handled" : "pending");
    return a;
}
bool code_review_can_edit_findings(const Json *review) {
    return review && !str_eq(json_str(json_get(review, "status")), "running");
}
// The item of `list` with `id`, which it owns; NULL when none.
static Json **slot_of(Json *list, int64_t id) {
    if (!json_is_array(list)) return NULL;
    for (size_t i = 0; i < list->a.count; i++) if (code_review_id(list->a.items[i]) == id) return &list->a.items[i];
    return NULL;
}
bool code_reviews_replace_review(Json *reviews, const Json *review) {
    Json **slot = review_valid(review) ? slot_of(reviews, code_review_id(review)) : NULL;
    if (!slot) return false;
    json_free(*slot); *slot = json_clone(review);
    return true;
}
bool code_reviews_replace_finding(Json *reviews, int64_t review_id, const Json *finding) {
    Json **review = finding_valid(finding) ? slot_of(reviews, review_id) : NULL;
    Json **slot = review ? slot_of((Json *)json_get(*review, "findings"), code_review_id(finding)) : NULL;
    if (!slot) return false;
    json_free(*slot); *slot = json_clone(finding);
    return true;
}

const char *const code_review_verdict_choices[CODE_REVIEW_VERDICT_CHOICES] = { "fix", "optional", "dismiss", NULL };
const char *const code_review_verification_choices[CODE_REVIEW_VERIFICATION_CHOICES] = { "confirmed", "plausible", NULL };
const char *code_review_verdict_title(const char *verdict) {
    if (str_eq(verdict, "fix")) return "Fix";
    if (str_eq(verdict, "optional")) return "Optional";
    return str_eq(verdict, "dismiss") ? "Dismissed" : "None";
}
const char *code_review_verification_title(const char *verification) {
    if (str_eq(verification, "confirmed")) return "Confirmed";
    return str_eq(verification, "plausible") ? "Plausible" : "None";
}
// The choice `value` names; an unknown value counts as none.
static const char *choice(const char *const *choices, size_t count, const char *value) {
    for (size_t i = 0; i + 1 < count; i++) if (str_eq(choices[i], value)) return choices[i];
    return NULL;
}
void code_review_edit_init(CodeReviewFindingEdit *edit, const Json *review, const Json *finding) {
    memset(edit, 0, sizeof *edit);
    edit->review_id = code_review_id(review); edit->finding_id = code_review_id(finding);
    edit->verdict = edit->saved_verdict = choice(code_review_verdict_choices, CODE_REVIEW_VERDICT_CHOICES, json_str(json_get(finding, "shouldFix")));
    edit->verification = edit->saved_verification = choice(code_review_verification_choices, CODE_REVIEW_VERIFICATION_CHOICES, json_str(json_get(finding, "verification")));
    const char *comments = json_str(json_get(finding, "comments"));
    edit->reason = xstrdup(comments ? comments : "");
    edit->saved_reason = str_trim(edit->reason);
}
void code_review_edit_free(CodeReviewFindingEdit *edit) {
    if (!edit) return;
    free(edit->reason); free(edit->saved_reason);
    memset(edit, 0, sizeof *edit);
}
void code_review_edit_choose_verdict(CodeReviewFindingEdit *edit, size_t index) {
    if (index < CODE_REVIEW_VERDICT_CHOICES) edit->verdict = code_review_verdict_choices[index];
}
void code_review_edit_choose_verification(CodeReviewFindingEdit *edit, size_t index) {
    if (index < CODE_REVIEW_VERIFICATION_CHOICES) edit->verification = code_review_verification_choices[index];
}
void code_review_edit_set_reason(CodeReviewFindingEdit *edit, const char *reason) {
    free(edit->reason); edit->reason = xstrdup(reason ? reason : "");
}
// Whether the trimmed reason equals `text`.
static bool reason_is(const CodeReviewFindingEdit *edit, const char *text) {
    char *said = str_trim(edit->reason); bool same = str_eq(said, text); free(said);
    return same;
}
static bool ruling_unchanged(const CodeReviewFindingEdit *edit) {
    return edit->verdict == edit->saved_verdict && edit->verification == edit->saved_verification;
}
bool code_review_edit_dirty(const CodeReviewFindingEdit *edit) {
    return !ruling_unchanged(edit) || !reason_is(edit, edit->saved_reason);
}
bool code_review_edit_needs_reason(const CodeReviewFindingEdit *edit) {
    return str_eq(edit->verdict, "optional") || str_eq(edit->verdict, "dismiss");
}
bool code_review_edit_has_reason(const CodeReviewFindingEdit *edit) {
    if (reason_is(edit, "")) return false;
    return edit->verdict == edit->saved_verdict || !reason_is(edit, edit->saved_reason);
}
bool code_review_edit_has_verification(const CodeReviewFindingEdit *edit) {
    if (str_eq(edit->verdict, "optional")) return edit->verification != NULL;
    if (str_eq(edit->verdict, "dismiss")) return str_eq(edit->verification, "confirmed");
    return true;
}
bool code_review_edit_can_save(const CodeReviewFindingEdit *edit) {
    if (!code_review_edit_dirty(edit)) return false;
    if (code_review_edit_needs_reason(edit) && !code_review_edit_has_reason(edit)) return false;
    return code_review_edit_has_verification(edit) || ruling_unchanged(edit);
}
const char *code_review_edit_reason_prompt(const CodeReviewFindingEdit *edit) {
    return code_review_edit_needs_reason(edit) ? "Why it will not be fixed" : "Why the verdict is what it is";
}
const char *code_review_edit_reason_hint(const CodeReviewFindingEdit *edit) {
    return code_review_edit_needs_reason(edit) ? "A finding left unfixed says why." : NULL;
}
const char *code_review_edit_verification_hint(const CodeReviewFindingEdit *edit) {
    if (code_review_edit_has_verification(edit)) return NULL;
    return str_eq(edit->verdict, "dismiss") ? "A dismissed finding has to be confirmed refuted." : "Say how well it is established.";
}
Json *code_review_edit_args(const CodeReviewFindingEdit *edit, const char *repo, int number) {
    Json *a = json_object();
    json_set_str(a, "repo", repo); json_set_num(a, "pr", number);
    json_set_num(a, "reviewId", (double)edit->review_id); json_set_num(a, "findingId", (double)edit->finding_id);
    if (edit->verdict != edit->saved_verdict) json_object_set(a, "shouldFix", json_string_or_null(edit->verdict));
    char *said = str_trim(edit->reason);
    if (!str_eq(said, edit->saved_reason)) json_object_set(a, "comments", json_string_or_null(*said ? said : NULL));
    free(said);
    if (edit->verification != edit->saved_verification) json_object_set(a, "verification", json_string_or_null(edit->verification));
    return a;
}
bool code_reviews_allow_saving(const Json *reviews, const CodeReviewFindingEdit *edit) {
    const Json *review = code_review_find(reviews, edit->review_id);
    if (!code_review_edit_can_save(edit) || !code_review_can_edit_findings(review)) return false;
    return code_review_finding(review, edit->finding_id) != NULL;
}
