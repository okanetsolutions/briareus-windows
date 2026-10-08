// Ordered, bounded thread mailbox. It owns no screen pointer or window handle.
#ifndef BRIAREUS_SLACK_FEED_H
#define BRIAREUS_SLACK_FEED_H
#include "api.h"
#include "slack.h"
typedef struct SlackFeed SlackFeed;
SlackFeed *slack_feed_new(ApiClient *client, const char *workspace, uint64_t generation);
bool slack_feed_start(SlackFeed *feed);
void slack_feed_stop(SlackFeed **feed);
/// The UI takes all pending events and the terminal error atomically.
bool slack_feed_take(SlackFeed *feed, SlackEvents *events, ApiError *error, uint64_t *generation);
/// Transport callback seam, also used by lifecycle tests without a provider connection.
void slack_feed_emit(void *feed, const char *name, const char *data, size_t length);
void slack_feed_end(SlackFeed *feed, const ApiError *error);
#endif
