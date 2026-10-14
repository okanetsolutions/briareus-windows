#ifndef BRIAREUS_PULL_CONVERSATION_H
#define BRIAREUS_PULL_CONVERSATION_H
#include "json.h"
#include <time.h>

typedef enum { CONV_COMMENTS, CONV_REVIEWS, CONV_REVIEW_COMMENTS, CONV_FEEDS } PullConversationFeed;
typedef struct { PullConversationFeed feed; size_t i; time_t at; } PullConversationEntry;

/// Feed arrays are borrowed; missing or non-array feeds are empty.
time_t pull_conversation_time(const Json *value, const char *field);
/// Follows numeric reply IDs, stopping at a missing parent, self-reference or 64 hops.
size_t pull_thread_root(const Json *lines, size_t index);
/// Finds a root's submitted review by its numeric ID, excluding pending reviews.
bool pull_thread_review(const Json *lines, const Json *reviews, size_t root, size_t *review);
/// Every conversation and line comment, plus each submitted review with a verdict or visible summary.
size_t pull_conversation_count(const Json *comments, const Json *reviews, const Json *lines);
/// Comments, submitted reviews and orphan threads, oldest first with ties by feed then index.
/// Returns an owned array released with free(); *count receives its length.
PullConversationEntry *pull_conversation_timeline(const Json *comments, const Json *reviews, const Json *lines, size_t *count);

#endif
