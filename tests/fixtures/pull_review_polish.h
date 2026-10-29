#ifndef BRIAREUS_PULL_REVIEW_POLISH_H
#define BRIAREUS_PULL_REVIEW_POLISH_H
#include "json.h"
#include "str.h"
#include "code_reviews.h"

// Entirely synthetic, shared by behavioral tests and the native preview harness.
static inline Json *pull_review_polish_fixture(void) {
    Json *response = json_parsez(CODE_REVIEWS_FIXTURE);
    Json *reviews = (Json *)json_get(response, "codeReviews");
    Json *review = (Json *)json_at(reviews, 0), *findings = (Json *)json_get(review, "findings");
    const char *titles[] = {
        "Keep line ranges together when the source path is long",
        "Preserve keyboard selection when results arrive out of order",
        "Keep a dismissed historical finding scoped to its commit",
        "Retain zero cost when the provider reports free usage",
        "Keep the inspector readable while scanning a long review"
    };
    for (size_t i = 0; i < 5; i++) {
        Json *f = json_clone(json_at(findings, i % 2));
        json_set_num(f, "id", 103 + (double)i); json_set_str(f, "title", titles[i]);
        json_set_str(f, "severity", i == 0 ? "critical" : i == 1 ? "high" : "low");
        json_set_str(f, "filePath", "app/screens/repository/inspection/background_refresh/selected_record_controller.c");
        json_set_num(f, "startLine", 1240); json_set_num(f, "endLine", 1258);
        json_set_str(f, "shouldFix", i == 2 ? "dismiss" : "optional");
        json_array_push(findings, f);
    }
    json_set_str((Json *)json_at(findings, 0), "body",
        "### Why this matters\nThe selected finding must stay readable during a **background refresh**. "
        "Keep the inspected record attached to its stable ID.\n\n"
        "- Preserve the current row\n- Reject obsolete responses\n\n"
        "[Implementation notes](https://example.com/synthetic/review/very/long/reference/selected_record_controller)\n\n"
        "```c\nconst char *selected_record = lookup_stable_record(snapshot, selected_review_id, selected_finding_id);\n```\n\n"
        "Additional evidence remains scrollable without moving the overview table.");
    Json *terminal = json_parsez(CODE_REVIEWS_FINAL_NULL_FIXTURE);
    json_array_push(reviews, json_clone(json_at(json_get(terminal, "codeReviews"), 0)));
    Json *zero = json_clone(json_at(reviews, 1));
    json_set_num(zero, "id", 19); json_set_num(zero, "inputTokens", 0); json_set_num(zero, "outputTokens", 0);
    json_set_num(zero, "costUsd", 0); json_set_num(zero, "unpricedTurns", 0); json_set_bool(zero, "costEstimated", false);
    json_object_set(zero, "findings", json_array()); json_object_remove(zero, "headSha");
    json_array_push(reviews, zero);
    json_free(terminal);
    return response;
}
#endif
