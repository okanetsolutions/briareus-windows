#include "screen_resources_internal.h"
#include "session_cache.h"

const char *resources_operation(ResourcesScreen *s) {
    switch (s->kind) {
    case RESOURCE_SERVER: return "server_stats";
    case RESOURCE_LIST: return "sessions_stats";
    default: return "session_stats";
    }
}
bool resources_adopt(ResourcesScreen *s, const Json *answer) {
    if (s->kind == RESOURCE_SERVER) {
        ServerStats next;
        if (!server_stats_parse(answer, &next)) return false;
        server_stats_free(&s->server); s->server = next; s->reading.host = next.host; return true;
    }
    if (s->kind == RESOURCE_LIST) {
        Project *projects = NULL; size_t count = 0, known_count = 0, missing = 0;
        Json *saved = g_store.cache ? cache_value(g_store.cache, "projects") : NULL;
        projects_parse(saved, &projects, &count); json_free(saved);
        Session *known = session_cache_read(projects, count, &known_count, &missing);
        projects_free(projects, count);
        OpenStats next; bool ok = open_stats_parse(answer, known, known_count, &next);
        sessions_free(known, known_count);
        if (!ok) return false;
        if (s->dashboard) s->dashboard->action_count = 0;
        open_stats_free(&s->sessions); s->sessions = next; s->reading.host = next.host; return true;
    }
    SessionStats next;
    if (!session_stats_parse(answer, session_id(&s->session), &next)) return false;
    session_stats_free(&s->reading); s->reading = next; return true;
}
