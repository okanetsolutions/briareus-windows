#include "screen_projects_internal.h"

void screen_projects_cache_all_sessions(const Session *sessions, size_t count) {
    Json *list = sessions_json(sessions, count);
    cache_store(g_store.cache, list, "sessions");
    json_free(list);
    Json *saved = cache_value(g_store.cache, "projects");
    Project *projects; size_t n;
    if (saved && projects_parse(saved, &projects, &n)) {
        for (size_t p = 0; p < n; p++) {
            list = json_array();
            for (size_t i = 0; i < count; i++) if (str_eq(session_repo(&sessions[i]), projects[p].repo)) json_array_push(list, json_clone(sessions[i].raw));
            char *key = xstrfmt("sessions:%s", projects[p].repo);
            cache_store(g_store.cache, list, key);
            json_free(list); free(key);
        }
        projects_free(projects, n);
    }
    json_free(saved);
}

static void forget_from_list(const char *key, const char *id) {
    Json *saved = cache_value(g_store.cache, key);
    Session *items; size_t n;
    if (saved && sessions_parse(saved, &items, &n)) {
        Json *list = json_array();
        for (size_t i = 0; i < n; i++) if (!str_eq(session_id(&items[i]), id)) json_array_push(list, json_clone(items[i].raw));
        cache_store(g_store.cache, list, key);
        json_free(list); sessions_free(items, n);
    }
    json_free(saved);
}

void screen_projects_forget_cached_session(const char *repo, const char *id) {
    forget_from_list("sessions", id);
    if (!str_empty(repo)) {
        char *key = xstrfmt("sessions:%s", repo);
        forget_from_list(key, id);
        free(key);
    }
    char *key = xstrfmt("transcript:%s", id);
    cache_remove(g_store.cache, key);
    free(key);
}
