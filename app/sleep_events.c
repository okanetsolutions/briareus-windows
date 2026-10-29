#include "sleep_internal.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

struct SleepFollow {
    StoreStream *stream;
    CRITICAL_SECTION lock;
    SleepInbox queue;
    SleepFollowInfo info;
    volatile LONG refs;
};
void sleep_follow_release(SleepFollow *f) {
    if (!f || InterlockedDecrement(&f->refs)) return;
    store_stream_free(f->stream); sleep_inbox_free(&f->queue); DeleteCriticalSection(&f->lock); free(f);
}
static void receive(void *ctx, const char *event, const char *data, size_t len) {
    SleepFollow *f = ctx;
    if (len > 65536) return;
    if (!str_eq(event, "session") && !str_eq(event, "session.deleted") && !str_eq(event, "transcript")) return;
    Json *j = json_parse(data, len); if (!j) return;
    EnterCriticalSection(&f->lock);
    bool queued = sleep_inbox_push(&f->queue, event, j);
    LeaveCriticalSection(&f->lock);
    if (!queued) store_stream_cancel(f->stream);
}
static void follow(void *ctx) {
    SleepFollow *f = ctx; ApiError error; api_error_init(&error);
    store_stream_read(f->stream, receive, f, &error);
    EnterCriticalSection(&f->lock); f->info.ended = true;
    f->info.status = error.status; f->info.retry_after = error.retry_after;
    LeaveCriticalSection(&f->lock);
    api_error_clear(&error); sleep_follow_release(f);
}
SleepFollow *sleep_follow_new(void) {
    ApiError error; api_error_init(&error); Json *args = json_object(); json_set_num(args, "transcripts", 1);
    StoreStream *stream = store_stream_new("events", args, &error); api_error_clear(&error);
    if (!stream) return NULL;
    SleepFollow *f = xcalloc(1, sizeof *f); f->stream = stream; f->refs = 2; InitializeCriticalSection(&f->lock);
    if (!async_try_run(follow, NULL, f)) { sleep_follow_release(f); sleep_follow_release(f); return NULL; }
    return f;
}
void sleep_follow_cancel(SleepFollow *f) { if (f) store_stream_cancel(f->stream); }
Json *sleep_follow_drain(SleepFollow *f, SleepFollowInfo *info) {
    EnterCriticalSection(&f->lock);
    *info = f->info; Json *queue = sleep_inbox_take(&f->queue, &info->reconcile);
    LeaveCriticalSection(&f->lock); return queue;
}
