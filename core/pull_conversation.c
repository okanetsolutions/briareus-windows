#include "pull_conversation.h"
#include "markdown.h"
#include "models.h"
#include "str.h"
#include <stdlib.h>

static size_t feed_count(const Json *list) { return json_is_array(list) ? json_count(list) : 0; }

time_t pull_conversation_time(const Json *value, const char *field) {
    time_t at;
    return board_date_parse(json_str(json_get(value, field)), &at) ? at : 0;
}
static bool find_id(const Json *list, const Json *id, size_t *out) {
    double want, have;
    if (!json_num(id, &want)) return false;
    for (size_t i = 0; i < feed_count(list); i++) {
        if (json_num(json_get(json_at(list, i), "id"), &have) && have == want) { *out = i; return true; }
    }
    return false;
}
size_t pull_thread_root(const Json *lines, size_t index) {
    size_t parent;
    for (int hop = 0; hop < 64 && find_id(lines, json_get(json_at(lines, index), "inReplyTo"), &parent) && parent != index; hop++) index = parent;
    return index;
}
bool pull_thread_review(const Json *lines, const Json *reviews, size_t root, size_t *review) {
    return find_id(reviews, json_get(json_at(lines, root), "reviewId"), review)
        && !str_ieq(json_str(json_get(json_at(reviews, *review), "state")), "pending");
}
static bool review_speaks(const Json *review) {
    const char *state = json_str(json_get(review, "state"));
    if (str_ieq(state, "pending")) return false;
    if (str_ieq(state, "approved") || str_ieq(state, "changes_requested") || str_ieq(state, "dismissed")) return true;
    const char *body = json_str(json_get(review, "body"));
    if (!body) return false;
    char *visible = visible_markdown(body);
    bool speaks = *visible != 0;
    free(visible);
    return speaks;
}
static bool review_has_threads(const Json *lines, const Json *reviews, size_t review) {
    size_t found;
    for (size_t i = 0; i < feed_count(lines); i++) {
        if (pull_thread_root(lines, i) == i && pull_thread_review(lines, reviews, i, &found) && found == review) return true;
    }
    return false;
}
size_t pull_conversation_count(const Json *comments, const Json *reviews, const Json *lines) {
    size_t count = feed_count(comments) + feed_count(lines);
    for (size_t i = 0; i < feed_count(reviews); i++) if (review_speaks(json_at(reviews, i))) count++;
    return count;
}
static int entry_compare(const void *a, const void *b) {
    const PullConversationEntry *x = a, *y = b;
    if (x->at != y->at) return x->at < y->at ? -1 : 1;
    if (x->feed != y->feed) return (int)x->feed - (int)y->feed;
    return x->i < y->i ? -1 : x->i > y->i;
}
PullConversationEntry *pull_conversation_timeline(const Json *comments, const Json *reviews, const Json *lines, size_t *count) {
    PullConversationEntry *entries = xcalloc(feed_count(comments) + feed_count(reviews) + feed_count(lines) + 1, sizeof *entries);
    *count = 0;
    for (size_t i = 0; i < feed_count(comments); i++) entries[(*count)++] = (PullConversationEntry){ CONV_COMMENTS, i, pull_conversation_time(json_at(comments, i), "createdAt") };
    for (size_t i = 0; i < feed_count(reviews); i++) {
        const Json *review = json_at(reviews, i);
        if (str_ieq(json_str(json_get(review, "state")), "pending")) continue;
        if (review_speaks(review) || review_has_threads(lines, reviews, i)) entries[(*count)++] = (PullConversationEntry){ CONV_REVIEWS, i, pull_conversation_time(review, "submittedAt") };
    }
    size_t review;
    for (size_t i = 0; i < feed_count(lines); i++) {
        if (pull_thread_root(lines, i) == i && !pull_thread_review(lines, reviews, i, &review)) entries[(*count)++] = (PullConversationEntry){ CONV_REVIEW_COMMENTS, i, pull_conversation_time(json_at(lines, i), "createdAt") };
    }
    qsort(entries, *count, sizeof *entries, entry_compare);
    return entries;
}
