#include "session_working.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>

static void append(Transcript *t, const char *text) {
    Json *j = json_parsez(text); CHECK(j != NULL);
    transcript_append(t, j); json_free(j);
}
static Session running(const char *tasks) {
    Session s = {0}; s.raw = json_object();
    json_set_str(s.raw, "id", "s1"); json_set_str(s.raw, "status", "running");
    if (tasks) json_object_set(s.raw, "backgroundTasks", json_parsez(tasks));
    return s;
}
static void parsing_and_text(void) {
    Transcript t; transcript_init(&t);
    append(&t, "[{\"seq\":1,\"kind\":\"result\"}]");
    const char *empty[] = {NULL, "null", "{}", "false", "42", "[]", "[null,3,{}, {\"name\":false}, {\"name\":\"\"}, {\"summary\":\"missing name\"}]"};
    for (size_t i = 0; i < sizeof empty / sizeof *empty; i++) {
        Session s = running(empty[i]);
        CHECK_OWNED_STR(session_working_text(&s, &t, "Thinking"), "Thinking\xE2\x80\xA6");
        session_free(&s);
    }
    Session s = running("[{}, {\"name\":\"Bash\",\"summary\":\"sleep 120\"}]");
    BackgroundTask task;
    CHECK(background_task_parse(json_at(json_get(s.raw, "backgroundTasks"), 1), &task));
    CHECK_STR(task.name, "Bash"); CHECK_STR(task.summary, "sleep 120");
    CHECK_OWNED_STR(session_working_text(&s, &t, "Thinking"), "Waiting on Bash: sleep 120\xE2\x80\xA6");
    Session copy; session_copy(&copy, &s); session_free(&s);
    CHECK_OWNED_STR(session_working_text(&copy, &t, "Working"), "Waiting on Bash: sleep 120\xE2\x80\xA6");
    Json *saved = sessions_json(&copy, 1); Session *restored = NULL; size_t n = 0;
    CHECK(sessions_parse(saved, &restored, &n)); CHECK_INT(n, 1); json_free(saved); session_free(&copy);
    CHECK_OWNED_STR(session_working_text(restored, &t, "Working"), "Waiting on Bash: sleep 120\xE2\x80\xA6");
    sessions_free(restored, n);
    const char *summaries[] = {"null", "5", "\"\""};
    for (size_t i = 0; i < 3; i++) {
        char *tasks = xstrfmt("[{\"name\":\"Monitor\",\"summary\":%s}]", summaries[i]);
        s = running(tasks); free(tasks);
        CHECK_OWNED_STR(session_working_text(&s, &t, "Working"), "Waiting on Monitor\xE2\x80\xA6");
        session_free(&s);
    }
    s = running("[{\"name\":\"Explore\"}, {}, {\"name\":\"general-purpose\"}]");
    CHECK_OWNED_STR(session_working_text(&s, &t, "Working"), "Waiting on 2 background tasks\xE2\x80\xA6");
    session_free(&s); transcript_free(&t);
}
static void latest_answer_and_boundaries(void) {
    const char *reset[] = {"user", "text", "ask", "tool", "tool_error", "status"};
    Session s = running("[{\"name\":\"Bash\"}]");
    Transcript t; transcript_init(&t); CHECK(!transcript_answered(&t));
    CHECK_OWNED_STR(session_working_text(&s, &t, "Working"), "Working\xE2\x80\xA6");
    append(&t, "[{\"seq\":1,\"kind\":\"text\",\"text\":\"Launching\"},{\"seq\":2,\"kind\":\"tool\"}]");
    CHECK(!transcript_answered(&t));
    CHECK_OWNED_STR(session_working_text(&s, &t, "Thinking"), "Thinking\xE2\x80\xA6");
    transcript_free(&t);
    for (size_t i = 0; i < sizeof reset / sizeof *reset; i++) {
        append(&t, "[{\"seq\":1,\"kind\":\"result\"},{\"seq\":2,\"kind\":\"info\",\"text\":\"PR #7 is open\"},{\"seq\":3,\"kind\":\"stderr\"},{\"seq\":4,\"kind\":\"info\",\"text\":\"No word for 30 min\"}]");
        CHECK(transcript_answered(&t));
        CHECK_OWNED_STR(session_working_text(&s, &t, "Working"), "Waiting on Bash\xE2\x80\xA6");
        char *next = xstrfmt("[{\"seq\":5,\"kind\":\"%s\",\"status\":\"running\"},{\"seq\":6,\"kind\":\"stderr\"}]", reset[i]);
        append(&t, next); free(next); CHECK(!transcript_answered(&t));
        CHECK_OWNED_STR(session_working_text(&s, &t, "Working"), "Working\xE2\x80\xA6");
        append(&t, "[{\"seq\":7,\"kind\":\"result\"}]"); CHECK(transcript_answered(&t));
        transcript_free(&t); CHECK(!transcript_answered(&t));
    }
    session_free(&s);
}
static void status_precedence(void) {
    Session s = running("[{\"name\":\"Bash\"}]");
    Transcript t; transcript_init(&t); append(&t, "[{\"seq\":1,\"kind\":\"result\"}]");
    const char *inactive[] = {"idle", "closed", "interrupted", "failed", "waiting", "unknown"};
    for (size_t i = 0; i < sizeof inactive / sizeof *inactive; i++) {
        json_set_str(s.raw, "status", inactive[i]); CHECK(session_working_text(&s, &t, "Working") == NULL);
    }
    json_set_str(s.raw, "status", "queued");
    CHECK_OWNED_STR(session_working_text(&s, &t, "Working"), "Queued\xE2\x80\xA6");
    const char *starting[] = {"starting", "preparing"};
    for (size_t i = 0; i < 2; i++) {
        json_set_str(s.raw, "status", starting[i]);
        CHECK_OWNED_STR(session_working_text(&s, &t, "Working"), "Starting up\xE2\x80\xA6");
    }
    json_set_str(s.raw, "status", "running"); json_set_bool(s.raw, "awaitingAnswer", true);
    CHECK(session_working_text(&s, &t, "Working") == NULL);
    json_set_bool(s.raw, "awaitingAnswer", false); json_object_remove(s.raw, "backgroundTasks");
    CHECK_OWNED_STR(session_working_text(&s, &t, "Thinking"), "Thinking\xE2\x80\xA6");
    session_free(&s); transcript_free(&t);
}
void session_working_tests(void) {
    test_run("background tasks parse tolerate old servers and survive model ownership", parsing_and_text);
    test_run("latest result ignores housekeeping but resets on turn activity", latest_answer_and_boundaries);
    test_run("working line respects question startup and terminal states", status_precedence);
}
