#include "sleep.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

int sleep_records_find(const SleepRecords *r, const char *id) {
    if (str_empty(id)) return -1;
    for (size_t i = 0; i < r->count; i++) if (str_eq(session_id(&r->items[i].session), id)) return (int)i;
    return -1;
}
static bool displayed(const SleepWall *wall, const char *id) {
    for (int i = 0; i < SLEEP_PANES; i++) if (str_eq(session_id(&wall->panes[i].session), id)) return true;
    return false;
}
static int priority(const SleepRecord *a, const SleepRecord *b) {
    bool aa = session_is_active(&a->session), ba = session_is_active(&b->session);
    if (aa != ba) return aa ? 1 : -1;
    if (aa && a->live != b->live) return a->live ? 1 : -1;
    if (aa && a->live && a->used != b->used) return a->used > b->used ? 1 : -1;
    int rank = sleep_session_priority(&a->session, &b->session);
    if (rank) return rank;
    return a->used > b->used ? 1 : a->used < b->used ? -1 : 0;
}
static int mark_find(const SleepRecords *r, const char *id) {
    for (size_t i = 0; i < r->mark_count; i++) if (str_eq(r->marks[i].id, id)) return (int)i;
    return -1;
}
static void remember(SleepRecords *r, const SleepRecord *record) {
    if (record->cursor <= 0) return;
    const char *id = session_id(&record->session); int at = mark_find(r, id);
    if (at < 0) {
        size_t slot = r->mark_count;
        if (slot == SLEEP_RECORDS) {
            slot = 0;
            for (size_t i = 1; i < r->mark_count; i++) if (r->marks[i].used < r->marks[slot].used) slot = i;
        } else r->mark_count++;
        free(r->marks[slot].id); r->marks[slot] = (SleepWatermark){xstrdup(id), 0, 0}; at = (int)slot;
    }
    if (record->cursor > r->marks[at].cursor) r->marks[at].cursor = record->cursor;
    r->marks[at].used = ++r->serial;
}
int sleep_records_put(SleepRecords *r, const Session *s, const SleepWall *wall, bool force) {
    if (str_empty(session_id(s)) || str_empty(session_repo(s))) return -1;
    int at = sleep_records_find(r, session_id(s));
    if (at >= 0) {
        SleepRecord *record = &r->items[at];
        const char *old = json_str(json_get(record->session.raw, "updatedAt"));
        const char *next = json_str(json_get(s->raw, "updatedAt"));
        if (old && next && strcmp(old, next) > 0) return -1;
        if (!json_equal(record->session.raw, s->raw)) {
            Session copy = {0}; session_copy(&copy, s); session_free(&record->session); record->session = copy;
            if (!session_is_active(s)) record->live = false;
        }
        return at;
    }
    // Capture an evicted identity's cursor before remembering the next victim can replace its cache entry.
    int mark = mark_find(r, session_id(s)); int cursor = mark >= 0 ? r->marks[mark].cursor : 0;
    if (mark >= 0) r->marks[mark].used = ++r->serial;
    SleepRecord incoming = {.session = *s, .used = r->serial + 1};
    if (r->count < SLEEP_RECORDS) at = (int)r->count++;
    else {
        for (size_t i = 0; i < r->count; i++) {
            if (displayed(wall, session_id(&r->items[i].session))) continue;
            if (at < 0 || priority(&r->items[i], &r->items[at]) < 0) at = (int)i;
        }
        if (at < 0 || (!force && priority(&incoming, &r->items[at]) < 0)) return -1;
        remember(r, &r->items[at]); session_free(&r->items[at].session);
    }
    SleepRecord *record = &r->items[at]; memset(record, 0, sizeof *record);
    session_copy(&record->session, s); record->used = ++r->serial;
    record->cursor = cursor;
    return at;
}
void sleep_records_touch(SleepRecords *r, int slot) {
    if (slot < 0 || (size_t)slot >= r->count) return;
    r->items[slot].used = ++r->serial; r->items[slot].live = session_is_active(&r->items[slot].session);
}
void sleep_records_delete(SleepRecords *r, const char *id) {
    int at = sleep_records_find(r, id), mark = mark_find(r, id);
    if (at >= 0) {
        session_free(&r->items[at].session); size_t tail = --r->count;
        if ((size_t)at != tail) r->items[at] = r->items[tail];
        memset(&r->items[tail], 0, sizeof r->items[tail]);
    }
    at = mark;
    if (at >= 0) {
        free(r->marks[at].id); size_t tail = --r->mark_count;
        if ((size_t)at != tail) r->marks[at] = r->marks[tail];
        memset(&r->marks[tail], 0, sizeof r->marks[tail]);
    }
}
void sleep_records_free(SleepRecords *r) {
    for (size_t i = 0; i < r->count; i++) session_free(&r->items[i].session);
    for (size_t i = 0; i < r->mark_count; i++) free(r->marks[i].id);
    memset(r, 0, sizeof *r);
}
void sleep_refresh_hint(SleepRefresh *r, uint64_t now) { if (r->due > now) r->due = now; }
bool sleep_refresh_due(const SleepRefresh *r, uint64_t now) { return now >= r->due && now >= r->not_before; }
void sleep_refresh_started(SleepRefresh *r, uint64_t now) { r->due = r->not_before = now + 30000; }
void sleep_refresh_finished(SleepRefresh *r, uint64_t now, bool accepted) {
    if (accepted) { r->failures = 0; return; }
    if (r->failures < 2) r->failures++;
    uint64_t due = now + (r->failures == 1 ? 30000 : 60000);
    if (due > r->not_before) r->not_before = due;
    r->due = r->not_before;
}
