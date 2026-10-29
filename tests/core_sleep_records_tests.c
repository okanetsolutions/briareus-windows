#include "sleep.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>

static Session row(int n, const char *status) {
    Session s = {json_object()}; char *id = xstrfmt("s%d", n);
    json_set_str(s.raw, "id", id); free(id); json_set_str(s.raw, "repo", "o/r"); json_set_str(s.raw, "status", status); return s;
}
static void active_recent_and_displayed_records_survive_archives(void) {
    SleepRecords r = {0}; SleepWall wall = {0};
    for (int k = 0; k < 220; k++) {
        Session s = row(k, k < 5 ? "running" : "closed");
        char *date = xstrfmt("2026-10-29T10:%02d:%02dZ", (219 - k) / 60, (219 - k) % 60);
        json_set_str(s.raw, "createdAt", date); free(date);
        int at = sleep_records_put(&r, &s, &wall, false);
        if (k < 4) sleep_assign(&wall, &s, 1000 + (uint64_t)k, false);
        if (k == 4 && at >= 0) { r.items[at].cursor = 42; sleep_records_touch(&r, at); }
        session_free(&s);
    }
    CHECK_INT(r.count, 128);
    for (int k = 0; k < 5; k++) { char *id = xstrfmt("s%d", k); CHECK(sleep_records_find(&r, id) >= 0); free(id); }
    CHECK(sleep_records_find(&r, "s127") >= 0); CHECK_INT(sleep_records_find(&r, "s219"), -1);
    int fifth = sleep_records_find(&r, "s4"); CHECK(fifth >= 0);
    if (fifth >= 0) CHECK_INT(r.items[fifth].cursor, 42);
    Session old = row(4, "running"); json_set_str(old.raw, "updatedAt", "2026-10-29T11:00:00Z");
    int at = sleep_records_put(&r, &old, &wall, false); CHECK(at >= 0);
    json_set_str(old.raw, "updatedAt", "2026-10-29T09:00:00Z"); CHECK_INT(sleep_records_put(&r, &old, &wall, false), -1);
    session_free(&old); sleep_records_free(&r); sleep_wall_free(&wall);
}
static void fair_victims_and_bounded_watermarks(void) {
    SleepRecords r = {0}; SleepWall wall = {0};
    for (int k = 0; k < 128; k++) {
        Session s = row(k, "running"); int at = sleep_records_put(&r, &s, &wall, false);
        if (at >= 0) r.items[at].cursor = 20 + k;
        if (k < 4) sleep_assign(&wall, &s, (uint64_t)k, false);
        session_free(&s);
    }
    Session incoming = row(1000, "running"); int first = sleep_records_put(&r, &incoming, &wall, true); session_free(&incoming);
    CHECK_INT(sleep_records_find(&r, "s4"), -1);
    incoming = row(4, "running"); int second = sleep_records_put(&r, &incoming, &wall, true); session_free(&incoming);
    CHECK(first != second); CHECK(second >= 0);
    if (second >= 0) CHECK_INT(r.items[second].cursor, 24);
    if (second >= 0) sleep_records_delete(&r, session_id(&r.items[second].session));
    incoming = row(4, "running");
    second = sleep_records_put(&r, &incoming, &wall, true); session_free(&incoming);
    if (second >= 0) CHECK_INT(r.items[second].cursor, 0);
    for (int k = 0; k < 300; k++) {
        incoming = row(2000 + k, "running"); int at = sleep_records_put(&r, &incoming, &wall, true); session_free(&incoming);
        if (at >= 0) r.items[at].cursor = 1000 + k;
        CHECK(r.count <= 128); CHECK(r.mark_count <= 128);
    }
    for (int k = 0; k < 4; k++) { char *id = xstrfmt("s%d", k); CHECK(sleep_records_find(&r, id) >= 0); free(id); }
    sleep_records_free(&r); sleep_wall_free(&wall); CHECK_INT(r.count, 0); CHECK_INT(r.mark_count, 0);
}
static void hints_and_rejected_snapshots_are_paced(void) {
    SleepRefresh r = {0}; CHECK(sleep_refresh_due(&r, 100)); sleep_refresh_started(&r, 100);
    for (uint64_t t = 101; t < 30100; t += 100) { sleep_refresh_hint(&r, t); CHECK(!sleep_refresh_due(&r, t)); }
    CHECK(sleep_refresh_due(&r, 30100)); sleep_refresh_started(&r, 30100);
    sleep_refresh_finished(&r, 30200, false); CHECK_INT(r.not_before, 60200);
    sleep_refresh_hint(&r, 31000); CHECK(!sleep_refresh_due(&r, 60199)); CHECK(sleep_refresh_due(&r, 60200));
    sleep_refresh_started(&r, 60200); sleep_refresh_finished(&r, 60300, false); CHECK_INT(r.not_before, 120300);
    sleep_refresh_hint(&r, 90000); CHECK(!sleep_refresh_due(&r, 120299)); CHECK(sleep_refresh_due(&r, 120300));
    sleep_refresh_started(&r, 120300); sleep_refresh_finished(&r, 120400, true); CHECK_INT(r.failures, 0);
    sleep_refresh_finished(&r, 120500, false); CHECK_INT(r.not_before, 150500);
}
static void full_watermark_cache_restores_oldest_identity_before_eviction(void) {
    SleepRecords r = {0}; SleepWall wall = {0};
    for (int k = 0; k < 128; k++) {
        Session s = row(k, "running"); int at = sleep_records_put(&r, &s, &wall, false);
        if (at >= 0) r.items[at].cursor = 20 + k;
        if (k < 4) sleep_assign(&wall, &s, (uint64_t)k, false);
        session_free(&s);
    }
    for (int k = 0; k < 128; k++) {
        Session s = row(1000 + k, "running"); int at = sleep_records_put(&r, &s, &wall, true);
        if (at >= 0) r.items[at].cursor = 1000 + k;
        session_free(&s);
    }
    CHECK_INT(r.mark_count, 128);
    Session s = row(4, "running"); int at = sleep_records_put(&r, &s, &wall, true); CHECK(at >= 0);
    if (at >= 0) CHECK_INT(r.items[at].cursor, 24);
    session_free(&s); sleep_records_free(&r); sleep_wall_free(&wall);
}
void sleep_records_tests(void) {
    test_run("sleep full watermark cache restores an evicted identity before saving the next victim", full_watermark_cache_restores_oldest_identity_before_eviction);
    test_run("sleep registry retains active recent and displayed records from newest-first archive", active_recent_and_displayed_records_survive_archives);
    test_run("sleep registry chooses fair victims and restores bounded sequence watermarks", fair_victims_and_bounded_watermarks);
    test_run("sleep unknown hints and stale snapshot retries share bounded pacing", hints_and_rejected_snapshots_are_paced);
}
