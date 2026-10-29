#include "sleep.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

static Session session(const char *id, const char *status, const char *branch) {
    Session s = {json_object()}; json_set_str(s.raw, "id", id); json_set_str(s.raw, "repo", "o/r");
    json_set_str(s.raw, "status", status); json_set_str(s.raw, "branch", branch); return s;
}
static void idle_and_gestures(void) {
    CHECK_INT(sleep_minutes(-1), 5); CHECK_INT(sleep_minutes(3), 5);
    for (int i = 0; i < 7; i++) CHECK_INT(sleep_minutes(sleep_choices[i]), sleep_choices[i]);
    SleepIdle s = {0}; CHECK(!sleep_due(&s, 299999, 5, true)); CHECK(sleep_due(&s, 300000, 5, true));
    CHECK(sleep_input(&s, 300001, 2, true, false)); CHECK(!s.asleep);
    CHECK(sleep_input(&s, 301000, 2, false, true)); // release long after wake is swallowed
    CHECK(!sleep_input(&s, 301001, 2, true, false));
    CHECK(!sleep_input(&s, 301002, 2, false, true));
    CHECK(!sleep_due(&s, 9999999, 0, true));
    CHECK(!sleep_due(&s, 9999999, 1, false)); CHECK(!sleep_due(&s, 10059998, 1, true));
    CHECK(sleep_due(&s, 10059999, 1, true));
    CHECK(sleep_input(&s, 10060000, 1, true, false));
    CHECK(sleep_input(&s, 10060010, 1, true, false)); // repeat and translated characters
    CHECK(sleep_input(&s, 10061000, 1, false, true));
    CHECK(!sleep_input(&s, 10061001, 1, true, false));
    CHECK(!sleep_input(&s, 10061002, 1, false, true));
    s.asleep = true; CHECK(sleep_input(&s, 11000000, 0, false, false)); // scroll
    CHECK(sleep_input(&s, 11000020, 0, false, false));
}
static void paths_and_tools(void) {
    CHECK_OWNED_STR(sleep_path("/w/o__r__3/src/a.c", "o/r"), "src/a.c");
    CHECK_OWNED_STR(sleep_path("././src//a.c", NULL), "src/a.c");
    const char *bad[] = {NULL, "", "../a", "a/../b", "/etc/passwd", "/w/o__r__x/a", "/w/o__r", "C:\\x", "https://x/a", "a\\b", "a\tb"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) { char *p = sleep_path(bad[i], "o/r"); CHECK(p == NULL); free(p); }
    Json *e = json_parsez("{\"kind\":\"tool\",\"name\":\"Edit\",\"summary\":\"src/a.c, src/b.c\"}");
    SleepStep step = sleep_step(e, "o/r"); CHECK_INT(step.kind, SLEEP_FILE); CHECK(step.writes); CHECK_STR(step.text, "src/a.c"); sleep_step_free(&step);
    json_set_str(e, "name", "Bash"); json_set_str(e, "summary", "make test\nignored");
    step = sleep_step(e, "o/r"); CHECK_INT(step.kind, SLEEP_COMMAND); CHECK_STR(step.text, "make test"); sleep_step_free(&step);
    json_set_str(e, "name", "Read"); json_set_str(e, "summary", "/w/o__r/a.c"); CHECK_INT(sleep_event_kind(e), SLEEP_FILE);
    json_set_str(e, "name", "Grep"); CHECK_INT(sleep_event_kind(e), SLEEP_NONE); step = sleep_step(e, "o/r"); CHECK_INT(step.kind, SLEEP_NONE); sleep_step_free(&step);
    json_set_str(e, "kind", "git"); json_set_str(e, "text", "git fetch"); step = sleep_step(e, "o/r"); CHECK_STR(step.text, "git fetch"); sleep_step_free(&step); json_free(e);
}
static void assignment_replay_and_ticker(void) {
    SleepWall w = {0}; Session a = session("a", "running", "dev"); int i = sleep_assign(&w, &a, 1000, true);
    CHECK(w.panes[i].replay); CHECK_INT(sleep_assign(&w, &a, 1001, true), i);
    int live = sleep_assign(&w, &a, 2000, false); CHECK(!w.panes[live].replay);
    SleepStep cmd = {SLEEP_COMMAND, false, "make test"}; sleep_apply(&w, live, &cmd, 2000, false);
    CHECK(!sleep_replay_due(&w, 61999)); CHECK(sleep_replay_due(&w, 62000));
    for (int k = 0; k < 40; k++) sleep_apply(&w, live, &cmd, 2100, false);
    CHECK_INT(w.commands, 30); CHECK_STR(w.ticker[29].project, "r"); CHECK_STR(w.panes[live].command, "make test");
    for (int k = 0; k < 4; k++) { char *id = xstrfmt("s%d", k); Session b = session(id, "running", NULL); sleep_assign(&w, &b, 3000 + (uint64_t)k, false); session_free(&b); free(id); }
    bool a_found = false; for (int k = 0; k < 4; k++) a_found |= str_eq(session_id(&w.panes[k].session), "a"); CHECK(!a_found);
    int remove = -1; for (int k = 0; k < 4; k++) if (str_eq(session_id(&w.panes[k].session), "s3")) remove = k;
    if (remove >= 0) sleep_remove(&w, session_id(&w.panes[remove].session));
    int occupied = 0; for (int k = 0; k < 4; k++) occupied += w.panes[k].session.raw != NULL; CHECK_INT(occupied, 3);
    sleep_wall_free(&w); CHECK_INT(w.commands, 0); session_free(&a);
}
static void file_revision_dwell_and_limits(void) {
    SleepWall w = {0}; Session a = session("a", "running", "feature/a"); int i = sleep_assign(&w, &a, 1, false); SleepPane *p = &w.panes[i];
    SleepStep file = {SLEEP_FILE, false, "a.c"}; sleep_apply(&w, i, &file, 2, false); uint64_t revision = p->revision;
    Json *args = sleep_file_args(p, false); CHECK_STR(json_str(json_get(args, "ref")), "feature/a"); CHECK_STR(json_str(json_get(args, "repo")), "o/r"); json_free(args);
    args = sleep_file_args(p, true); CHECK(json_is_null(json_get(args, "ref"))); CHECK_STR(json_str(json_get(args, "path")), "a.c"); json_free(args);
    file.text = "b.c"; sleep_apply(&w, i, &file, 3, false); CHECK_STR(p->path, "a.c"); CHECK_STR(p->pending, "b.c");
    sleep_tick(&w, 6002); CHECK_STR(p->path, "b.c"); CHECK(!sleep_loaded(p, revision, NULL));
    Json *answer = json_parsez("{\"path\":\"b.c\",\"content\":\"int x = 1;\\n\"}");
    CHECK(sleep_loaded(p, p->revision, answer)); CHECK(p->loaded); CHECK_STR(p->content, "int x = 1;\n");
    sleep_tick(&w, 6003); CHECK_INT(p->typed, 7);
    file.text = "c.c"; file.writes = true; sleep_apply(&w, i, &file, 6004, false); CHECK_STR(p->path, "c.c");
    revision = p->revision; json_set_str(a.raw, "branch", "other"); sleep_assign(&w, &a, 6005, false); CHECK(!sleep_loaded(p, revision, answer)); CHECK(p->path == NULL);
    file.text = "big"; sleep_apply(&w, i, &file, 6006, false);
    char *large = xmalloc(SLEEP_TEXT + 2); memset(large, 'x', SLEEP_TEXT + 1); large[SLEEP_TEXT + 1] = 0;
    json_set_str(answer, "content", large); free(large); CHECK(sleep_loaded(p, p->revision, answer)); CHECK(p->content == NULL);
    json_set_str(answer, "content", "x"); json_set_bool(answer, "binary", true); CHECK(sleep_loaded(p, p->revision, answer)); CHECK(p->content == NULL);
    json_free(answer); sleep_wall_free(&w); session_free(&a);
}
static void selection(void) {
    Session items[6]; const char *states[] = {"idle", "running", "closed", "running", "idle", "running"};
    for (int i = 0; i < 6; i++) { char *id = xstrfmt("s%d", i); items[i] = session(id, states[i], NULL); free(id); }
    json_set_str(items[1].raw, "updatedAt", "2026-10-29T10:00:00Z");
    json_set_str(items[3].raw, "updatedAt", "2026-10-29T11:00:00Z");
    json_set_str(items[5].raw, "updatedAt", "2026-10-29T09:00:00Z");
    size_t out[4]; CHECK_INT(sleep_select(items, 6, out, true), 3); CHECK_INT(out[0], 3); CHECK_INT(out[1], 1); CHECK_INT(out[2], 5);
    CHECK_INT(sleep_select(items, 6, out, false), 4); CHECK_INT(out[3], 0);
    json_set_str(items[2].raw, "updatedAt", "2026-10-29T12:00:00Z");
    CHECK_INT(sleep_select(items, 6, out, false), 4); CHECK_INT(out[0], 2);
    for (int i = 0; i < 6; i++) session_free(&items[i]);
}
static void replay_round_robin(void) {
    SleepReplay r = {0}; int pane;
    CHECK(sleep_replay_next(&r, &pane) == NULL);
    Json *a = json_parsez("[{\"kind\":\"cmd\",\"text\":\"one\"},{\"kind\":\"text\",\"text\":\"ignored\"},{\"kind\":\"cmd\",\"text\":\"two\"}]");
    Json *b = json_parsez("[{\"kind\":\"cmd\",\"text\":\"other\"}]");
    sleep_replay_load(&r, 0, a, "o/r"); sleep_replay_load(&r, 2, b, "x/y");
    const SleepStep *step = sleep_replay_next(&r, &pane); CHECK_INT(pane, 0); CHECK_STR(step->text, "one");
    step = sleep_replay_next(&r, &pane); CHECK_INT(pane, 2); CHECK_STR(step->text, "other");
    step = sleep_replay_next(&r, &pane); CHECK_INT(pane, 0); CHECK_STR(step->text, "two");
    step = sleep_replay_next(&r, &pane); CHECK_INT(pane, 2); CHECK_STR(step->text, "other");
    step = sleep_replay_next(&r, &pane); CHECK_INT(pane, 0); CHECK_STR(step->text, "one");
    for (int i = 0; i < 80; i++) { Json *e = json_object(); json_set_str(e, "kind", "cmd"); char *text = xstrfmt("cmd%d", i); json_set_str(e, "text", text); free(text); json_array_push(b, e); }
    sleep_replay_load(&r, 1, b, "x/y"); CHECK_INT(r.count[1], 40); CHECK_STR(r.steps[1][0].text, "cmd40"); CHECK_STR(r.steps[1][39].text, "cmd79");
    for (int i = 0; i < SLEEP_PANES; i++) sleep_replay_clear(&r, i);
    CHECK(sleep_replay_next(&r, &pane) == NULL); json_free(a); json_free(b);
}

static void seed_behind_live_text(void) {
    SleepWall w = {0}; Session s = session("s", "running", "dev"); int i = sleep_assign(&w, &s, 1000, false);
    w.panes[i].cursor = 100; w.last_live = 1500;
    Json *events = json_parsez("[{\"seq\":98,\"kind\":\"cmd\",\"text\":\"make test\"},{\"seq\":99,\"kind\":\"tool\",\"name\":\"Edit\",\"summary\":\"a.c\"},{\"seq\":100,\"kind\":\"text\",\"text\":\"working\"}]");
    sleep_seed(&w, i, events, 2000); CHECK_STR(w.panes[i].path, "a.c"); CHECK_STR(w.panes[i].command, "make test"); CHECK_INT(w.last_live, 1500);
    SleepStep file = {SLEEP_FILE, true, "new.c"}; sleep_apply(&w, i, &file, 9000, false); w.panes[i].file_cursor = 105;
    sleep_seed(&w, i, events, 9001); CHECK_STR(w.panes[i].path, "new.c");
    json_free(events); session_free(&s); sleep_wall_free(&w);
}

static void pointer_positions(void) {
    SleepPointer p = {0}; CHECK(!sleep_pointer_moved(&p, 10, 20));
    CHECK(!sleep_pointer_moved(&p, 10, 20)); CHECK(sleep_pointer_moved(&p, 11, 20));
    CHECK(sleep_pointer_moved(&p, 11, 21)); CHECK(!sleep_pointer_moved(&p, 11, 21));
    sleep_pointer_reset(&p, -50, 200); CHECK(!sleep_pointer_moved(&p, -50, 200));
    CHECK(sleep_pointer_moved(&p, -49, 200));
}
static void bounded_connect_backlog(void) {
    SleepInbox q = {0};
    for (int k = 0; k < 220; k++) {
        Json *s = json_object(); char *id = xstrfmt("closed%d", k); json_set_str(s, "id", id); free(id);
        json_set_str(s, "status", "closed"); CHECK(sleep_inbox_push(&q, "session", s)); CHECK(q.count <= SLEEP_BACKLOG);
    }
    CHECK_INT(q.count, 128); CHECK(q.reconcile);
    CHECK(sleep_inbox_push(&q, "session.deleted", json_parsez("{\"id\":\"deleted\"}")));
    for (int k = 1; k <= 3; k++) {
        Json *e = json_object(); json_set_num(e, "seq", k); CHECK(sleep_inbox_push(&q, "transcript", e));
    }
    bool reconcile = false; Json *batch = sleep_inbox_take(&q, &reconcile);
    CHECK(reconcile); CHECK_INT(json_count(batch), 128); CHECK_INT(q.count, 0);
    CHECK_STR(json_str(json_get(json_at(batch, 124), "type")), "session.deleted");
    for (int k = 1; k <= 3; k++) CHECK_INT(json_int_or(json_get(json_get(json_at(batch, 124 + (size_t)k), "data"), "seq"), 0), k);
    json_free(batch);
    for (int k = 0; k < 128; k++) CHECK(sleep_inbox_push(&q, "transcript", json_number(k)));
    CHECK(sleep_inbox_push(&q, "session", json_object())); // a snapshot alone cannot force a reconnect
    CHECK(!sleep_inbox_push(&q, "transcript", json_number(128)));
    CHECK_INT(q.count, 128); sleep_inbox_free(&q); CHECK_INT(q.count, 0);
}
static void newest_active_connect_snapshots_survive(void) {
    SleepInbox q = {0};
    for (int k = 0; k < 220; k++) {
        Json *s = json_object(); char *id = xstrfmt("s%d", k);
        char *date = xstrfmt("2026-10-29T10:%02d:%02dZ", (219 - k) / 60, (219 - k) % 60);
        json_set_str(s, "id", id); json_set_str(s, "repo", "o/r");
        json_set_str(s, "status", k < 5 ? "running" : "closed"); json_set_str(s, "createdAt", date);
        CHECK(sleep_inbox_push(&q, "session", s)); free(id); free(date);
    }
    bool reconcile; Json *batch = sleep_inbox_take(&q, &reconcile); CHECK(reconcile); CHECK_INT(json_count(batch), 128);
    for (int k = 0; k < 5; k++) {
        char *id = xstrfmt("s%d", k); bool found = false;
        for (size_t i = 0; i < json_count(batch); i++) if (str_eq(json_str(json_get(json_get(json_at(batch, i), "data"), "id")), id)) found = true;
        CHECK(found); free(id);
    }
    CHECK_STR(json_str(json_get(json_get(json_at(batch, 127), "data"), "id")), "s127");
    json_free(batch); sleep_inbox_free(&q);
}
void sleep_tests(void) {
    test_run("sleep newest-first backlog retains active sessions and recent archive", newest_active_connect_snapshots_survive);
    test_run("sleep bounded distinct connect records retain ordered live events", bounded_connect_backlog);
    test_run("sleep ignores stationary synthetic pointer moves and resets baseline", pointer_positions);
    test_run("sleep history seeds behind live text without rewinding live file", seed_behind_live_text);
    test_run("sleep bounded replay round robin", replay_round_robin);
    test_run("sleep idle and wake gestures", idle_and_gestures);
    test_run("sleep safe tool paths", paths_and_tools);
    test_run("sleep assignments replay takeover and bounded ticker", assignment_replay_and_ticker);
    test_run("sleep file revision branch dwell and limits", file_revision_dwell_and_limits);
    test_run("sleep selection recency", selection);
}
