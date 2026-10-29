#include "sleep.h"
#include "str.h"
#include <string.h>

static void remove_at(SleepInbox *q, size_t at) {
    json_free(q->events[at]);
    memmove(q->events + at, q->events + at + 1, (q->count - at - 1) * sizeof *q->events);
    q->count--;
}
bool sleep_inbox_push(SleepInbox *q, const char *type, Json *data) {
    bool session = str_eq(type, "session");
    if (!session && !str_eq(type, "transcript") && !str_eq(type, "session.deleted")) { json_free(data); return true; }
    if (q->count == SLEEP_BACKLOG) {
        size_t at = 0;
        while (at < q->count && !str_eq(json_str(json_get(q->events[at], "type")), "session")) at++;
        q->reconcile = true;
        if (at == q->count) { json_free(data); return session; }
        // Session records are snapshots, so discard one before sacrificing any ordered live event or deletion.
        remove_at(q, at);
    }
    Json *packet = json_object(); json_set_str(packet, "type", type); json_object_set(packet, "data", data);
    q->events[q->count++] = packet; return true;
}
Json *sleep_inbox_take(SleepInbox *q, bool *reconcile) {
    Json *out = json_array();
    for (size_t i = 0; i < q->count; i++) json_array_push(out, q->events[i]);
    *reconcile = q->reconcile; memset(q, 0, sizeof *q); return out;
}
void sleep_inbox_free(SleepInbox *q) {
    for (size_t i = 0; i < q->count; i++) json_free(q->events[i]);
    memset(q, 0, sizeof *q);
}
