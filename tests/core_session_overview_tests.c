#include "session_overview.h"
#include "suites.h"
#include "test.h"
#include "str.h"
#include <stdlib.h>

static void states_and_order(void) {
    Json *raw = json_parsez("["
        "{\"id\":\"wait\",\"status\":\"idle\",\"awaitingAnswer\":true,\"endedAt\":\"2026-01-02T00:00:00Z\",\"reviewTriage\":{\"findings\":[]}},"
        "{\"id\":\"held\",\"status\":\"idle\",\"reviewLoop\":{\"triage\":{\"findings\":[]}},\"createdAt\":\"2026-01-01T00:00:00Z\"},"
        "{\"id\":\"beta\",\"status\":\"running\",\"title\":\"beta\",\"awaitingAnswer\":true},"
        "{\"id\":\"alpha\",\"status\":\"preparing\",\"title\":\"Alpha\"},"
        "{\"id\":\"ready\",\"status\":\"idle\",\"parentId\":\"\"},"
        "{\"id\":\"failed\",\"status\":\"failed\"},{\"id\":\"closed\",\"status\":\"closed\"},"
        "{\"id\":\"child-wait\",\"status\":\"idle\",\"awaitingAnswer\":true,\"parentId\":\"wait\"},"
        "{\"id\":\"child-work\",\"status\":\"running\",\"parentId\":\"wait\"},"
        "{\"id\":\"child-ready\",\"status\":\"idle\",\"parentId\":\"wait\"},"
        "{\"id\":\"queued\",\"status\":\"queued\",\"title\":\"Zulu\"},"
        "{\"id\":\"starting\",\"status\":\"starting\",\"title\":\"Zulu\"},"
        "{\"id\":\"other\",\"status\":\"interrupted\"}]");
    Session *items; size_t count; CHECK(sessions_parse(raw, &items, &count)); json_free(raw);
    CHECK_INT(session_state(&items[0]), SESSION_WAITING); CHECK_INT(session_state(&items[1]), SESSION_FINDINGS);
    CHECK_INT(session_state(&items[2]), SESSION_WORKING); CHECK_INT(session_state(&items[4]), SESSION_READY);
    CHECK_INT(session_state(&items[5]), SESSION_FAILED); CHECK_INT(session_state(&items[6]), SESSION_CLOSED);
    CHECK_INT(session_state(&items[12]), SESSION_CLOSED);
    SessionOverview o = session_overview(items, count);
    CHECK_INT(o.waiting_count, 2); CHECK_INT(o.working_count, 4); CHECK_INT(o.ready, 1);
    CHECK_STR(session_id(&items[o.waiting[0]]), "held"); CHECK_STR(session_id(&items[o.waiting[1]]), "wait");
    CHECK_STR(session_id(&items[o.working[0]]), "alpha"); CHECK_STR(session_id(&items[o.working[1]]), "beta");
    CHECK_STR(session_id(&items[o.working[2]]), "queued"); CHECK_STR(session_id(&items[o.working[3]]), "starting");
    CHECK_STR(session_state_label(SESSION_FINDINGS), "Findings to decide");
    session_overview_free(&o); sessions_free(items, count);
    o = session_overview(NULL, 0); CHECK_INT(o.waiting_count, 0); CHECK_INT(o.working_count, 0); CHECK_INT(o.ready, 0); session_overview_free(&o);
}
static void question_lifetime(void) {
    Transcript t = {0}; CHECK(transcript_pending_question(&t) == NULL);
    Json *events = json_parsez("[{\"seq\":2,\"kind\":\"ask\",\"question\":\"Which?\",\"options\":[{\"label\":\"A\"},{\"label\":\"B\"},{},{}]},{\"seq\":3,\"kind\":\"status\",\"text\":\"idle\"}]");
    transcript_append(&t, events); json_free(events);
    const Event *ask = transcript_pending_question(&t); CHECK(ask != NULL); if (!ask) { transcript_free(&t); return; } CHECK_INT(ask->seq, 2); CHECK_STR(ask->question, "Which?");
    CHECK_STR(json_str(json_get(json_at(ask->options, 0), "label")), "A");
    Json *identity = json_clone(ask->raw);
    events = json_parsez("[{\"seq\":4,\"kind\":\"user\",\"text\":\"\"}]"); transcript_append(&t, events); json_free(events); CHECK(transcript_pending_question(&t) == NULL);
    events = json_parsez("[{\"seq\":5,\"kind\":\"ask\",\"text\":\"Which?\"}]"); transcript_append(&t, events); json_free(events);
    ask = transcript_pending_question(&t); CHECK_INT(ask->seq, 5); CHECK(!json_equal(identity, ask->raw)); CHECK_STR(ask->text, "Which?");
    json_free(identity); transcript_free(&t); CHECK(transcript_pending_question(&t) == NULL);
    Session s; Json *raw = json_parsez("{\"id\":\"a\",\"repo\":\"one\",\"status\":\"idle\",\"awaitingAnswer\":true}"); CHECK(session_parse(raw, &s)); json_free(raw);
    identity = session_question_identity(&s); json_set_str(s.raw, "endedAt", "2026-01-02T00:00:00Z"); Json *next = session_question_identity(&s); CHECK(!json_equal(identity, next)); json_free(next);
    json_set_str(s.raw, "repo", "two"); next = session_question_identity(&s); CHECK(!json_equal(identity, next)); json_free(next); json_free(identity); session_free(&s);
}
void core_session_overview_tests(void) {
    test_run("Status counts and sorts top-level session states with question precedence", states_and_order);
    test_run("Status pending question is cleared by user messages and reset with transcript", question_lifetime);
}
