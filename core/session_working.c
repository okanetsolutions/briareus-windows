#include "session_working.h"
#include "str.h"

bool background_task_parse(const Json *value, BackgroundTask *out) {
    out->name = json_str_nonempty(json_get(value, "name"));
    out->summary = json_str_nonempty(json_get(value, "summary"));
    return out->name != NULL;
}

bool transcript_answered(const Transcript *transcript) {
    for (size_t i = transcript->count; i > 0; i--) {
        const char *kind = transcript->events[i - 1].kind;
        if (str_eq(kind, "result")) return true;
        if (str_eq(kind, "user") || str_eq(kind, "text") || str_eq(kind, "ask")) return false;
        if (str_eq(kind, "tool") || str_eq(kind, "tool_error") || str_eq(kind, "status")) return false;
    }
    return false;
}

char *session_working_text(const Session *session, const Transcript *transcript, const char *verb) {
    if (!session_is_active(session)) return NULL;
    if (json_bool_is(json_get(session->raw, "awaitingAnswer"), true)) return NULL;
    const char *status = session_status(session);
    if (str_eq(status, "queued")) return xstrdup("Queued\xE2\x80\xA6");
    if (str_eq(status, "preparing") || str_eq(status, "starting")) return xstrdup("Starting up\xE2\x80\xA6");
    const Json *tasks = json_get(session->raw, "backgroundTasks");
    if (json_is_array(tasks) && transcript_answered(transcript)) {
        BackgroundTask first = {0}; size_t count = 0;
        for (size_t i = 0; i < json_count(tasks); i++) {
            BackgroundTask task;
            if (!background_task_parse(json_at(tasks, i), &task)) continue;
            if (!count) first = task;
            count++;
        }
        if (count > 1) return xstrfmt("Waiting on %zu background tasks\xE2\x80\xA6", count);
        if (count) return xstrfmt("Waiting on %s%s%s\xE2\x80\xA6", first.name, first.summary ? ": " : "", first.summary ? first.summary : "");
    }
    return xstrfmt("%s\xE2\x80\xA6", verb);
}
