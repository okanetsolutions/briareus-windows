#include "pull_conversation.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>

static void test_thread_roots(void) {
    Json *lines = json_parsez("[{\"id\":10},{\"id\":11,\"inReplyTo\":10},{\"id\":12,\"inReplyTo\":11},"
                              "{\"id\":13,\"inReplyTo\":99},{\"id\":14,\"inReplyTo\":14},{\"id\":15,\"inReplyTo\":\"10\"}]");
    CHECK_INT(pull_thread_root(lines, 0), 0);
    CHECK_INT(pull_thread_root(lines, 2), 0);
    CHECK_INT(pull_thread_root(lines, 3), 3);
    CHECK_INT(pull_thread_root(lines, 4), 4);
    CHECK_INT(pull_thread_root(lines, 5), 5);
    CHECK_INT(pull_thread_root(NULL, 0), 0);
    json_free(lines);
}
static void test_thread_hop_limit(void) {
    Json *cycle = json_parsez("[{\"id\":1,\"inReplyTo\":2},{\"id\":2,\"inReplyTo\":3},{\"id\":3,\"inReplyTo\":1}]");
    CHECK_INT(pull_thread_root(cycle, 0), 1);
    CHECK_INT(pull_thread_root(cycle, 1), 2);
    CHECK_INT(pull_thread_root(cycle, 2), 0);
    size_t count;
    PullConversationEntry *entries = pull_conversation_timeline(NULL, NULL, cycle, &count);
    CHECK_INT(count, 0);
    CHECK_INT(pull_conversation_count(NULL, NULL, cycle), 3);
    free(entries); json_free(cycle);
    Json *chain = json_array();
    for (int i = 0; i < 70; i++) {
        Json *line = json_object();
        json_object_set(line, "id", json_number(i));
        if (i < 69) json_object_set(line, "inReplyTo", json_number(i + 1));
        json_array_push(chain, line);
    }
    CHECK_INT(pull_thread_root(chain, 0), 64);
    CHECK_INT(pull_thread_root(chain, 68), 69);
    json_free(chain);
}
static void test_review_association(void) {
    Json *reviews = json_parsez("[{\"id\":20,\"state\":\"COMMENTED\"},{\"id\":21,\"state\":\"PeNdInG\"}]");
    Json *lines = json_parsez("[{\"id\":1,\"reviewId\":20},{\"id\":2,\"inReplyTo\":1,\"reviewId\":21},"
                              "{\"id\":3,\"reviewId\":21},{\"id\":4,\"reviewId\":22},{\"id\":5,\"reviewId\":\"20\"}]");
    size_t review;
    CHECK(pull_thread_review(lines, reviews, 0, &review)); CHECK_INT(review, 0);
    CHECK(pull_thread_review(lines, reviews, pull_thread_root(lines, 1), &review)); CHECK_INT(review, 0);
    CHECK(!pull_thread_review(lines, reviews, 2, &review));
    CHECK(!pull_thread_review(lines, reviews, 3, &review));
    CHECK(!pull_thread_review(lines, reviews, 4, &review));
    CHECK(!pull_thread_review(NULL, reviews, 0, &review));
    json_free(lines); json_free(reviews);
}
static void test_counts_visible_reviews(void) {
    Json *comments = json_parsez("[{},{}]");
    Json *lines = json_parsez("[{},{}]");
    Json *reviews = json_parsez("[{\"state\":\"approved\"},{\"state\":\"CHANGES_REQUESTED\"},{\"state\":\"dismissed\"},"
                                "{\"state\":\"pending\",\"body\":\"Hidden pending summary\"},{\"body\":\"Summary\"},"
                                "{\"body\":\"  \\n\\t\"},{\"body\":\"<!-- internal -->\\n<badge>\"},"
                                "{\"body\":\"<details>\\n<summary>Title</summary>\\nReview text\\n</details>\"},"
                                "{\"body\":\"```html\\n<code>\\n```\"},{\"body\":7},{}]");
    CHECK_INT(pull_conversation_count(comments, reviews, lines), 10);
    CHECK_INT(pull_conversation_count(NULL, reviews, NULL), 6);
    json_free(comments); json_free(lines); json_free(reviews);
}
static void test_timeline_groups_reviews_and_orphans(void) {
    Json *reviews = json_parsez("[{\"id\":20,\"state\":\"commented\"},{\"id\":21,\"state\":\"pending\",\"body\":\"Summary\"},"
                                "{\"id\":22,\"state\":\"approved\"},{\"id\":23,\"body\":\"<!-- hidden -->\"}]");
    Json *lines = json_parsez("[{\"id\":1,\"reviewId\":20},{\"id\":2,\"inReplyTo\":1},"
                              "{\"id\":3,\"reviewId\":21},{\"id\":4,\"reviewId\":99}]");
    size_t count;
    PullConversationEntry *entries = pull_conversation_timeline(NULL, reviews, lines, &count);
    CHECK_INT(count, 4);
    if (count == 4) {
        CHECK_INT(entries[0].feed, CONV_REVIEWS); CHECK_INT(entries[0].i, 0);
        CHECK_INT(entries[1].feed, CONV_REVIEWS); CHECK_INT(entries[1].i, 2);
        CHECK_INT(entries[2].feed, CONV_REVIEW_COMMENTS); CHECK_INT(entries[2].i, 2);
        CHECK_INT(entries[3].feed, CONV_REVIEW_COMMENTS); CHECK_INT(entries[3].i, 3);
    }
    CHECK_INT(pull_conversation_count(NULL, reviews, lines), 5);
    free(entries); json_free(lines); json_free(reviews);
}
static void test_timeline_chronology_and_ties(void) {
    Json *comments = json_parsez("[{\"createdAt\":\"2026-10-14T12:00:00Z\"},{\"createdAt\":\"2026-10-14T12:00:00Z\"},"
                                 "{\"createdAt\":\"2026-10-14T12:01:00Z\"},{\"createdAt\":\"invalid\"}]");
    Json *reviews = json_parsez("[{\"state\":\"approved\",\"submittedAt\":\"2026-10-14T12:00:00Z\"},"
                                "{\"state\":\"approved\",\"submittedAt\":\"2026-10-14T11:59:00Z\"}]");
    Json *lines = json_parsez("[{\"createdAt\":\"2026-10-14T12:00:00Z\"}]");
    size_t count;
    PullConversationEntry *entries = pull_conversation_timeline(comments, reviews, lines, &count);
    CHECK_INT(count, 7);
    if (count == 7) {
        CHECK_INT(entries[0].feed, CONV_COMMENTS); CHECK_INT(entries[0].i, 3); CHECK_INT(entries[0].at, 0);
        CHECK_INT(entries[1].feed, CONV_REVIEWS); CHECK_INT(entries[1].i, 1);
        CHECK_INT(entries[2].feed, CONV_COMMENTS); CHECK_INT(entries[2].i, 0);
        CHECK_INT(entries[3].feed, CONV_COMMENTS); CHECK_INT(entries[3].i, 1);
        CHECK_INT(entries[4].feed, CONV_REVIEWS); CHECK_INT(entries[4].i, 0);
        CHECK_INT(entries[5].feed, CONV_REVIEW_COMMENTS); CHECK_INT(entries[5].i, 0);
        CHECK_INT(entries[6].feed, CONV_COMMENTS); CHECK_INT(entries[6].i, 2);
        CHECK(entries[1].at < entries[2].at); CHECK(entries[5].at < entries[6].at);
    }
    free(entries); json_free(comments); json_free(reviews); json_free(lines);
}
static void test_empty_and_malformed_feeds(void) {
    Json *object = json_parsez("{\"not\":\"a feed\"}");
    Json *malformed = json_parsez("[null,7,\"unknown\",{\"body\":false}]");
    size_t count;
    PullConversationEntry *entries = pull_conversation_timeline(NULL, object, object, &count);
    CHECK_INT(count, 0); CHECK_INT(pull_conversation_count(object, object, NULL), 0);
    free(entries);
    entries = pull_conversation_timeline(NULL, malformed, NULL, &count);
    CHECK_INT(count, 0); CHECK_INT(pull_conversation_count(NULL, malformed, NULL), 0);
    CHECK_INT(pull_conversation_time(object, "createdAt"), 0);
    CHECK_INT(pull_conversation_time(NULL, "createdAt"), 0);
    free(entries); json_free(object); json_free(malformed);
}
void pull_conversation_tests(void) {
    test_run("pull threads follow numeric reply chains", test_thread_roots);
    test_run("pull thread traversal caps cycles and long chains", test_thread_hop_limit);
    test_run("pull threads associate with submitted root reviews", test_review_association);
    test_run("pull conversation counts visible submitted review messages", test_counts_visible_reviews);
    test_run("pull timeline groups reviews and preserves orphan threads", test_timeline_groups_reviews_and_orphans);
    test_run("pull timeline sorts chronologically with deterministic ties", test_timeline_chronology_and_ties);
    test_run("pull conversation accepts empty and malformed feeds", test_empty_and_malformed_feeds);
}
