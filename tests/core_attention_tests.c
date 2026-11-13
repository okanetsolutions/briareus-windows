#include "attention.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>

static void the_pulls_waiting_on_a_person_are_read_in_order(void) {
    Json *answer = json_parsez("{\"items\":["
        "{\"id\":\"q1\",\"kind\":\"question\",\"sessionId\":\"s1\",\"repo\":\"o/r\",\"title\":\"Which?\"},"
        "{\"id\":\"a1\",\"kind\":\"review-approval\",\"sessionId\":null,\"repo\":\"o/r\",\"prNumber\":12,\"title\":\"Fix the guard\","
        "\"summary\":\"High risk: 1 more approval\",\"at\":\"2026-11-12T10:00:00Z\"},"
        "{\"id\":\"l1\",\"kind\":\"review-limit\",\"repo\":\"o/r\",\"prNumber\":13},"
        "{\"id\":\"a2\",\"kind\":\"review-approval\",\"repo\":\"o/s\",\"prNumber\":4},"
        "{\"id\":\"a3\",\"kind\":\"review-approval\",\"sessionId\":\"s2\",\"repo\":\"o/r\",\"prNumber\":5},"
        "{\"id\":\"a4\",\"kind\":\"review-approval\",\"prNumber\":6},"
        "{\"id\":\"a5\",\"kind\":\"review-approval\",\"repo\":\"o/r\",\"prNumber\":1.5},"
        "\"junk\"]}");
    size_t n; AttentionPull *pulls = attention_approval_pulls(answer, &n);
    CHECK_INT(n, 2);
    CHECK_STR(pulls[0].id, "a1"); CHECK_STR(pulls[0].repo, "o/r"); CHECK_INT(pulls[0].number, 12);
    CHECK_STR(pulls[0].title, "Fix the guard"); CHECK_STR(pulls[0].summary, "High risk: 1 more approval"); CHECK_STR(pulls[0].at, "2026-11-12T10:00:00Z");
    CHECK_STR(pulls[1].repo, "o/s"); CHECK_INT(pulls[1].number, 4); CHECK(pulls[1].title == NULL && pulls[1].summary == NULL);
    CHECK(!attention_pull_same(&pulls[0], &pulls[1]));
    AttentionPull same = {.repo = "o/r", .number = 12}; CHECK(attention_pull_same(&pulls[0], &same));
    attention_pulls_free(pulls, n); json_free(answer);
    // Anything but a list of items reads as none.
    answer = json_parsez("{\"items\":null}"); pulls = attention_approval_pulls(answer, &n); CHECK_INT(n, 0);
    attention_pulls_free(pulls, n); json_free(answer);
    attention_pulls_free(NULL, 0);
}
void core_attention_tests(void) {
    test_run("attention reads the pull requests waiting on a person's approval", the_pulls_waiting_on_a_person_are_read_in_order);
}
