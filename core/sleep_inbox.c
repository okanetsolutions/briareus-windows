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
        size_t at = q->count;
        bool same = false;
        for (size_t i = 0; i < q->count; i++) {
            if (!str_eq(json_str(json_get(q->events[i], "type")), "session")) continue;
            Session candidate = {(Json *)json_get(q->events[i], "data")};
            if (session && str_eq(session_id(&candidate), json_str(json_get(data, "id")))) same = true;
            if (at == q->count) at = i;
            else {
                Session worst = {(Json *)json_get(q->events[at], "data")};
                if (sleep_session_priority(&candidate, &worst) < 0) at = i;
            }
        }
        q->reconcile = true;
        if (at == q->count) { json_free(data); return session; }
        if (session && !same) {
            Session incoming = {data}, worst = {(Json *)json_get(q->events[at], "data")};
            if (sleep_session_priority(&incoming, &worst) < 0) { json_free(data); return true; }
        }
        // Preserve active/recent metadata even when the server sends its newest sessions first.
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
