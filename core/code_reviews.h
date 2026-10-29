#ifndef BRIAREUS_CODE_REVIEWS_H
#define BRIAREUS_CODE_REVIEWS_H
#include "json.h"
#include <stdint.h>

// Owns a clone of the public codeReviews array; malformed data returns NULL, never an empty success.
Json *code_reviews_parse(const Json *response);
int64_t code_review_id(const Json *record);
// Borrowed records, independent of the lifetime of review sessions.
const Json *code_review_find(const Json *reviews, int64_t id);
const Json *code_review_finding(const Json *review, int64_t id);
size_t code_reviews_findings(const Json *reviews);
bool code_reviews_running(const Json *reviews);
// "current head", "older head" or "head unknown"; never equates two unknown heads.
const char *code_review_head(const Json *review, const char *current_head);
// Null verdicts await work only while their parent review is running.
const char *code_review_verdict(const Json *review, const Json *finding);
const char *code_review_verification(const Json *review, const Json *finding);
// Owned display strings; null usage remains visibly unknown.
char *code_review_location(const Json *finding);
char *code_review_usage(const Json *review);
char *code_review_counts(const Json *review);
char *code_reviews_counts(const Json *reviews);
// Values are sums of known nonnegative observations; missing values never become zero.
typedef struct { double value; size_t known, unknown; } CodeReviewAmount;
typedef struct {
    size_t reviews, findings, unpriced_turns;
    CodeReviewAmount input, output, cost;
    bool estimated;
} CodeReviewTotals;
CodeReviewTotals code_review_totals(const Json *review);
CodeReviewTotals code_reviews_totals(const Json *reviews);
char *code_review_counts_compact(const Json *review);
#endif
