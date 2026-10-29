#include "code_reviews.h"
#include "str.h"
#include <math.h>
#include <stdlib.h>

int64_t code_review_id(const Json *record) {
    double n;
    if (!json_num(json_get(record, "id"), &n) || n < 1 || n > 9007199254740991.0 || floor(n) != n) return 0;
    return (int64_t)n;
}
Json *code_reviews_parse(const Json *response) {
    const Json *reviews = json_get(response, "codeReviews");
    if (reviews->type != JSON_ARRAY || json_count(reviews) > 20) return NULL;
    for (size_t i = 0; i < json_count(reviews); i++) {
        const Json *r = json_at(reviews, i), *findings = json_get(r, "findings");
        const char *status = json_str(json_get(r, "status"));
        if (!code_review_id(r) || findings->type != JSON_ARRAY) return NULL;
        if (!str_eq(status, "running") && !str_eq(status, "completed") && !str_eq(status, "failed")) return NULL;
        for (size_t j = 0; j < i; j++) if (code_review_id(json_at(reviews, j)) == code_review_id(r)) return NULL;
        for (size_t j = 0; j < json_count(findings); j++) {
            const Json *f = json_at(findings, j);
            const char *severity = json_str(json_get(f, "severity"));
            if (!code_review_id(f) || !json_str(json_get(f, "title"))) return NULL;
            if (!str_eq(severity, "critical") && !str_eq(severity, "high") && !str_eq(severity, "medium") && !str_eq(severity, "low")) return NULL;
            for (size_t k = 0; k < j; k++) if (code_review_id(json_at(findings, k)) == code_review_id(f)) return NULL;
        }
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
