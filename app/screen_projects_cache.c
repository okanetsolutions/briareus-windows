#include "screen_projects_internal.h"

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
    if (!str_empty(repo)) {
        char *key = xstrfmt("sessions:%s", repo);
        forget_from_list(key, id);
        free(key);
    }
    char *key = xstrfmt("transcript:%s", id);
    cache_remove(g_store.cache, key);
    free(key);
}
