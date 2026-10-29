#include "ssh_servers.h"
#include "str.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

char *ssh_server_key(const Json *row) {
    double id;
    if (json_num(json_get(row, "id"), &id) && isfinite(id)) return xstrfmt("ssh:%.0f", id);
    return xstrfmt("ssh:%s", json_str(json_get(row, "host")) ? json_str(json_get(row, "host")) : "");
}
char *ssh_server_name(const Json *row) {
    const char *label = json_str_nonempty(json_get(row, "label"));
    if (label) return xstrdup(label);
    const char *user = json_str(json_get(row, "username")), *host = json_str(json_get(row, "host"));
    return user && *user ? xstrfmt("%s@%s", user, host ? host : "") : xstrdup(host ? host : "SSH server");
}
static const char *repo_of(const Json *row) {
    const char *repo = json_str(json_get(row, "repo"));
    return repo ? repo : "";
}
static bool listed(const Project *projects, size_t count, const char *repo) {
    for (size_t i = 0; i < count; i++) if (str_eq(projects[i].repo, repo)) return true;
    return false;
}
static void folder(Json *out, const Json *rows, const char *repo, const char *title, const char *q) {
    Json *indices = json_array();
    bool project_match = *repo && (str_icontains(repo, q) || str_icontains(title, q));
    for (size_t i = 0; i < json_count(rows); i++) {
        const Json *r = json_at(rows, i);
        if (!str_eq(repo_of(r), repo)) continue;
        bool match = !*q || project_match || str_icontains(json_str(json_get(r, "label")), q);
        match = match || str_icontains(json_str(json_get(r, "host")), q) || str_icontains(json_str(json_get(r, "username")), q);
        if (match) json_array_push(indices, json_number((double)i));
    }
    if (!json_count(indices)) { json_free(indices); return; }
    Json *f = json_object(); json_set_str(f, "repo", repo); json_set_str(f, "title", title);
    json_object_set(f, "indices", indices); json_array_push(out, f);
}
static int repo_compare(const void *a, const void *b) { return _stricmp(*(const char *const *)a, *(const char *const *)b); }
Json *ssh_server_folders(const Json *rows, const Project *projects, size_t count, const char *query) {
    Json *out = json_array(); char *q = str_trim(query ? query : "");
    for (size_t i = 0; i < count; i++) {
        if (str_empty(projects[i].repo)) continue;
        bool duplicate = listed(projects, i, projects[i].repo);
        if (!duplicate) folder(out, rows, projects[i].repo, project_title(&projects[i]), q);
    }
    const char **extra = xcalloc(json_count(rows) + 1, sizeof *extra); size_t n = 0;
    for (size_t i = 0; i < json_count(rows); i++) {
        const char *repo = repo_of(json_at(rows, i));
        if (!*repo || listed(projects, count, repo)) continue;
        bool duplicate = false;
        for (size_t k = 0; k < n; k++) if (str_eq(extra[k], repo)) duplicate = true;
        if (!duplicate) extra[n++] = repo;
    }
    qsort(extra, n, sizeof *extra, repo_compare);
    for (size_t i = 0; i < n; i++) folder(out, rows, extra[i], extra[i], q);
    folder(out, rows, "", "No project", q);
    free(extra); free(q); return out;
}
bool ssh_server_folder_open(const Json *folded, const char *repo, const char *query) {
    char *q = str_trim(query ? query : ""); bool searching = *q != 0; free(q);
    return searching || !json_bool_is(json_get(folded, repo), true);
}
void ssh_server_folder_toggle(Json *folded, const char *repo, const char *query) {
    char *q = str_trim(query ? query : ""); bool searching = *q != 0; free(q);
    if (!searching) json_set_bool(folded, repo, !json_bool_is(json_get(folded, repo), true));
}
