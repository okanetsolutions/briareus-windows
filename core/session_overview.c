#include "session_overview.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

SessionState session_state(const Session *s) {
    if (str_eq(session_status(s), "idle")) {
        if (json_bool_is(json_get(s->raw, "awaitingAnswer"), true)) return SESSION_WAITING;
        if (session_held_round(s)) return SESSION_FINDINGS;
        return SESSION_READY;
    }
    if (session_is_active(s)) return SESSION_WORKING;
    return str_eq(session_status(s), "failed") ? SESSION_FAILED : SESSION_CLOSED;
}
bool session_needs_you(const Session *s) { SessionState state = session_state(s); return state == SESSION_WAITING || state == SESSION_FINDINGS; }
const char *session_state_label(SessionState state) {
    static const char *const labels[] = { "Needs your answer", "Findings to decide", "Working", "Ready", "Failed", "Closed" };
    return labels[state];
}
static time_t waiting_since(const Session *s) {
    time_t t;
    if (board_date_parse(json_str(json_get(s->raw, "endedAt")), &t)) return t;
    return board_date_parse(json_str(json_get(s->raw, "createdAt")), &t) ? t : 0;
}
SessionOverview session_overview(const Session *sessions, size_t count) {
    SessionOverview o = {0};
    o.waiting = xcalloc(count, sizeof *o.waiting); o.working = xcalloc(count, sizeof *o.working);
    for (size_t i = 0; i < count; i++) {
        const Session *s = &sessions[i];
        if (json_str_nonempty(json_get(s->raw, "parentId"))) continue;
        if (session_needs_you(s)) {
            size_t at = o.waiting_count++;
            while (at && waiting_since(&sessions[o.waiting[at - 1]]) > waiting_since(s)) { o.waiting[at] = o.waiting[at - 1]; at--; }
            o.waiting[at] = i;
        } else if (session_state(s) == SESSION_WORKING) {
            size_t at = o.working_count++;
            while (at && _stricmp(session_display_title(&sessions[o.working[at - 1]]), session_display_title(s)) > 0) { o.working[at] = o.working[at - 1]; at--; }
            o.working[at] = i;
        } else if (session_state(s) == SESSION_READY) o.ready++;
    }
    return o;
}
void session_overview_free(SessionOverview *o) { free(o->waiting); free(o->working); memset(o, 0, sizeof *o); }
const Event *transcript_pending_question(const Transcript *t) {
    for (size_t i = t->count; i > 0; i--) {
        const Event *e = &t->events[i - 1];
        if (str_eq(e->kind, "user")) return NULL;
        if (str_eq(e->kind, "ask")) return e;
    }
    return NULL;
}
Json *session_question_identity(const Session *s) {
    Json *identity = json_object();
    static const char *const keys[] = { "id", "repo", "status", "awaitingAnswer", "endedAt", "createdAt" };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) json_object_set(identity, keys[i], json_clone(json_get(s->raw, keys[i])));
    return identity;
}
