#include "session_cache.h"
#include "session_overview.h"
#include "str.h"
#include <stdlib.h>

Session *session_cache_read(const Project *projects, size_t count, size_t *out_count, size_t *missing) {
    Session *all = NULL; *out_count = 0; *missing = 0;
    for (size_t p = 0; p < count; p++) {
        char *key = xstrfmt("sessions:%s", projects[p].repo);
        Json *saved = cache_value(g_store.cache, key); free(key);
        Session *items = NULL; size_t n = 0;
        if (!saved || !sessions_parse(saved, &items, &n)) (*missing)++;
        for (size_t i = 0; i < n; i++) {
            if (!str_eq(session_repo(&items[i]), projects[p].repo)) continue;
            all = xrealloc(all, (*out_count + 1) * sizeof *all);
            session_copy(&all[(*out_count)++], &items[i]);
        }
        sessions_free(items, n); json_free(saved);
    }
    return all;
}
void session_cache_save(const Project *projects, size_t count, const Session *sessions, size_t n) {
    for (size_t p = 0; p < count; p++) {
        Json *list = json_array();
        for (size_t i = 0; i < n; i++) if (str_eq(session_repo(&sessions[i]), projects[p].repo)) json_array_push(list, json_clone(sessions[i].raw));
        char *key = xstrfmt("sessions:%s", projects[p].repo);
        cache_store(g_store.cache, list, key); free(key); json_free(list);
    }
}
size_t status_waiting(const Project *projects, size_t count) {
    size_t n, missing;
    Session *items = session_cache_read(projects, count, &n, &missing);
    SessionOverview o = session_overview(items, n); size_t waiting = o.waiting_count;
    session_overview_free(&o); sessions_free(items, n); return waiting;
}
