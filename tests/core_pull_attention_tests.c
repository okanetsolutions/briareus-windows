#include "pull_attention.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>

static void the_pull_request_items_are_kept_in_order(void) {
    Json *j = json_parsez("{\"items\":["
        "{\"id\":\"q1\",\"kind\":\"question\",\"repo\":\"o/r\",\"sessionId\":\"s\",\"title\":\"A question\"},"
        "{\"id\":\"a1\",\"kind\":\"review-approval\",\"repo\":\"o/r\",\"prNumber\":12,\"title\":\"Fix the guard\",\"summary\":\"High risk: 1 of 2 approvals\",\"href\":\"https://github.com/o/r/pull/12\",\"at\":\"2026-11-13T09:00:00Z\"},"
        "{\"id\":\"l1\",\"kind\":\"review-limit\",\"repo\":\"o/s\",\"prNumber\":4},"
        "{\"id\":\"s1\",\"kind\":\"review-status\",\"repo\":\"o/r\",\"prNumber\":7,\"title\":\"Docs\"},"
        "{\"id\":\"x1\",\"kind\":\"review-approval\",\"prNumber\":3},{\"id\":\"x2\",\"kind\":\"review-limit\",\"repo\":\"o/r\"},"
        "{\"id\":\"x3\",\"kind\":\"review-status\",\"repo\":\"o/r\",\"prNumber\":0}]}");
    PullAttention *items; size_t count;
    CHECK(pull_attention_parse(j, &items, &count)); CHECK_INT(count, 3);
    if (count == 3) {
        CHECK_INT(items[0].kind, PULL_ATTENTION_APPROVAL); CHECK_STR(items[0].repo, "o/r"); CHECK_INT(items[0].number, 12);
        CHECK_STR(items[0].title, "Fix the guard"); CHECK_STR(items[0].summary, "High risk: 1 of 2 approvals");
        CHECK_STR(items[0].href, "https://github.com/o/r/pull/12"); CHECK_STR(items[0].at, "2026-11-13T09:00:00Z");
        // A missing title names the pull request; a missing summary stays absent.
        CHECK_INT(items[1].kind, PULL_ATTENTION_LIMIT); CHECK(items[1].title && *items[1].title); CHECK(items[1].summary == NULL);
        CHECK_INT(items[2].kind, PULL_ATTENTION_STATUS); CHECK_STR(items[2].id, "s1");
    }
    pull_attention_free(items, count); json_free(j);
    CHECK(*pull_attention_kind_title(PULL_ATTENTION_APPROVAL));
    CHECK(*pull_attention_kind_title(PULL_ATTENTION_LIMIT)); CHECK(*pull_attention_kind_title(PULL_ATTENTION_STATUS));
}
static void an_answer_without_items_is_not_read(void) {
    PullAttention *items; size_t count;
    Json *j = json_parsez("{\"items\":null}"); CHECK(!pull_attention_parse(j, &items, &count)); CHECK(items == NULL); CHECK_INT(count, 0); json_free(j);
    j = json_parsez("{\"items\":[]}"); CHECK(pull_attention_parse(j, &items, &count)); CHECK_INT(count, 0); pull_attention_free(items, count); json_free(j);
    CHECK(!pull_attention_parse(NULL, &items, &count));
    pull_attention_free(NULL, 0);
}
void core_pull_attention_tests(void) {
    test_run("attention keeps the pull request items waiting on a person, in order", the_pull_request_items_are_kept_in_order);
    test_run("an attention answer without an item list is not read", an_answer_without_items_is_not_read);
}
