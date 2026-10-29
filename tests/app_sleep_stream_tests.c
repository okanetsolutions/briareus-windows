#include "../app/sleep_internal.h"
#include "screen_fixture.h"
#include "suites.h"
#include "test.h"

typedef struct { HANDLE burst, resume, live, finish; int opens, cancels, frees; } StreamFixture;
static StreamFixture transport;
static StoreStream *fake_stream_new(const char *operation, Json *args, ApiError *error) {
    (void)error; CHECK_STR(operation, "events"); CHECK_INT(json_int_or(json_get(args, "transcripts"), 0), 1);
    json_free(args); transport.opens++; return (StoreStream *)&transport;
}
static bool fake_stream_read(StoreStream *stream, SseEmit emit, void *ctx, ApiError *error) {
    (void)stream;
    for (int k = 0; k < 220; k++) {
        char *data = xstrfmt("{\"id\":\"closed%d\",\"repo\":\"o/r\",\"status\":\"closed\"}", k);
        emit(ctx, "session", data, strlen(data)); free(data);
    }
    SetEvent(transport.burst); WaitForSingleObject(transport.resume, 10000);
    const char *data = "{\"id\":\"live\",\"repo\":\"o/r\",\"status\":\"running\"}";
    emit(ctx, "session", data, strlen(data));
    for (int k = 1; k <= 2; k++) {
        char *line = xstrfmt("{\"sessionId\":\"live\",\"event\":{\"seq\":%d,\"kind\":\"cmd\",\"text\":\"live\"}}", k);
        emit(ctx, "transcript", line, strlen(line)); free(line);
    }
    data = "{\"id\":\"closed200\"}"; emit(ctx, "session.deleted", data, strlen(data));
    SetEvent(transport.live); WaitForSingleObject(transport.finish, 10000);
    api_error_set(error, API_CANCELLED, 0, NULL, 0); return false;
}
static void fake_stream_cancel(StoreStream *stream) {
    (void)stream; transport.cancels++; SetEvent(transport.resume); SetEvent(transport.finish);
}
static void fake_stream_free(StoreStream *stream) { (void)stream; transport.frees++; }
#define store_stream_new fake_stream_new
#define store_stream_read fake_stream_read
#define store_stream_cancel fake_stream_cancel
#define store_stream_free fake_stream_free
#define sleep_follow_new probe_follow_new
#define sleep_follow_release probe_follow_release
#define sleep_follow_cancel probe_follow_cancel
#define sleep_follow_drain probe_follow_drain
SleepFollow *probe_follow_new(void);
void probe_follow_release(SleepFollow *f);
void probe_follow_cancel(SleepFollow *f);
Json *probe_follow_drain(SleepFollow *f, SleepFollowInfo *info);
#include "../app/sleep_events.c"

static void connect_archive_does_not_cancel_stream(void) {
    ScreenFixture f; screen_fixture_init(&f, NULL, 0); memset(&transport, 0, sizeof transport);
    transport.burst = CreateEventW(NULL, TRUE, FALSE, NULL); transport.resume = CreateEventW(NULL, TRUE, FALSE, NULL);
    transport.live = CreateEventW(NULL, TRUE, FALSE, NULL); transport.finish = CreateEventW(NULL, TRUE, FALSE, NULL);
    SleepFollow *followed = probe_follow_new(); CHECK(followed != NULL);
    CHECK_INT(WaitForSingleObject(transport.burst, 10000), WAIT_OBJECT_0);
    SleepFollowInfo info; Json *batch = probe_follow_drain(followed, &info);
    CHECK_INT(json_count(batch), SLEEP_BACKLOG); CHECK(info.reconcile); CHECK(!info.ended);
    CHECK_INT(transport.cancels, 0); CHECK_INT(transport.opens, 1); json_free(batch);
    SetEvent(transport.resume); CHECK_INT(WaitForSingleObject(transport.live, 10000), WAIT_OBJECT_0);
    batch = probe_follow_drain(followed, &info);
    CHECK(!info.ended); CHECK(!info.reconcile); CHECK_INT(json_count(batch), 4);
    CHECK_STR(json_str(json_get(json_at(batch, 0), "type")), "session");
    for (int k = 1; k <= 2; k++) {
        const Json *packet = json_at(batch, (size_t)k);
        CHECK_STR(json_str(json_get(packet, "type")), "transcript");
        CHECK_INT(json_int_or(json_get(json_get(json_get(packet, "data"), "event"), "seq"), 0), k);
    }
    CHECK_STR(json_str(json_get(json_at(batch, 3), "type")), "session.deleted"); json_free(batch);
    CHECK_INT(transport.cancels, 0); probe_follow_cancel(followed); probe_follow_release(followed);
    ULONGLONG until = GetTickCount64() + 10000; MSG msg; bool completed = false;
    while (!completed && GetTickCount64() < until) {
        if (PeekMessageW(&msg, f.dispatcher, WM_APP_ASYNC_DONE, WM_APP_ASYNC_DONE, PM_REMOVE)) {
            store_handle_message(msg.message, msg.wParam, msg.lParam); completed = true;
        } else Sleep(1);
    }
    CHECK(completed); CHECK_INT(transport.frees, 1);
    CloseHandle(transport.burst); CloseHandle(transport.resume); CloseHandle(transport.live); CloseHandle(transport.finish);
    screen_fixture_clear(&f);
}
void app_sleep_stream_tests(void) { test_run("sleep real stream adapter survives 220 distinct connect records and delivers live FIFO", connect_archive_does_not_cancel_stream); }
