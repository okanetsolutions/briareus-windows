#include "slack_feed.h"
#include "str.h"
#include <stdlib.h>

struct SlackFeed {
    volatile LONG refs;
    CRITICAL_SECTION lock;
    ApiStreamCancel cancel;
    ApiClient *client;
    char *workspace;
    uint64_t generation;
    SlackEvents events;
    ApiError error;
    bool stopped, ended;
};
static void release(SlackFeed *f) {
    if (InterlockedDecrement(&f->refs)) return;
    slack_events_clear(&f->events); api_error_clear(&f->error);
    api_stream_cancel_free(&f->cancel); DeleteCriticalSection(&f->lock);
    api_client_release(f->client); free(f->workspace); free(f);
}
SlackFeed *slack_feed_new(ApiClient *client, const char *workspace, uint64_t generation) {
    SlackFeed *f = xcalloc(1, sizeof *f); f->refs = 1;
    InitializeCriticalSection(&f->lock); api_stream_cancel_init(&f->cancel); api_error_init(&f->error);
    f->client = api_client_retain(client); f->workspace = xstrdup(workspace); f->generation = generation;
    return f;
}
void slack_feed_emit(void *ctx, const char *name, const char *data, size_t length) {
    SlackFeed *f = ctx;
    EnterCriticalSection(&f->lock);
    bool overflow = !f->stopped && !slack_events_push(&f->events, name, data, length);
    LeaveCriticalSection(&f->lock);
    // Never silently discard an event. End this connection and recover from new snapshots.
    if (overflow) api_stream_cancel(&f->cancel);
}
void slack_feed_end(SlackFeed *f, const ApiError *error) {
    EnterCriticalSection(&f->lock);
    f->ended = true; api_error_copy(&f->error, error);
    LeaveCriticalSection(&f->lock);
}
static DWORD WINAPI read_stream(LPVOID ctx) {
    SlackFeed *f = ctx; Json *args = json_object(); json_set_str(args, "id", f->workspace);
    ApiError error; api_error_init(&error);
    api_stream(f->client, "slack_events", args, &f->cancel, slack_feed_emit, f, &error);
    json_free(args); slack_feed_end(f, &error); api_error_clear(&error); release(f); return 0;
}
bool slack_feed_start(SlackFeed *f) {
    InterlockedIncrement(&f->refs);
    HANDLE thread = CreateThread(NULL, 0, read_stream, f, 0, NULL);
    if (!thread) { release(f); return false; }
    CloseHandle(thread); return true;
}
void slack_feed_stop(SlackFeed **slot) {
    SlackFeed *f = *slot; *slot = NULL; if (!f) return;
    EnterCriticalSection(&f->lock); f->stopped = true; LeaveCriticalSection(&f->lock);
    api_stream_cancel(&f->cancel); release(f);
}
bool slack_feed_take(SlackFeed *f, SlackEvents *events, ApiError *error, uint64_t *generation) {
    EnterCriticalSection(&f->lock);
    *events = f->events; f->events = (SlackEvents){0};
    *generation = f->generation; bool ended = f->ended;
    if (ended) api_error_copy(error, &f->error);
    LeaveCriticalSection(&f->lock); return ended;
}
