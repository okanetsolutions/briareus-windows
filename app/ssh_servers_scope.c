#include "project_ssh_internal.h"
#include "str.h"
#include <stdlib.h>

static ApiClient *session_account;
static char *session_scope;

char *project_ssh_scope(void) {
    Str s; str_init(&s);
    str_appendf(&s, "%s\n%s", g_store.device.id ? g_store.device.id : "", g_store.device.permission ? g_store.device.permission : "");
    str_appendf(&s, "\n%d", project_ssh_offered());
    for (size_t i = 0; i < g_store.device.repo_count; i++) str_appendf(&s, "\n%s", g_store.device.repos[i]);
    return str_detach(&s);
}
bool project_ssh_current(ProjectSsh *p) {
    char *scope = project_ssh_scope();
    bool ok = store_connected() && p->account == g_store.client && str_eq(p->scope, scope);
    free(scope); return ok;
}
void servers_ssh_sessions_bind(void) {
    if (session_account) return;
    session_account = g_store.client; api_client_retain(session_account);
    session_scope = project_ssh_scope();
}
void servers_ssh_sessions_reset(void) {
    while (term_count()) term_close(term_at(0));
    api_client_release(session_account); session_account = NULL;
    free(session_scope); session_scope = NULL;
}
void servers_ssh_sessions_check(void) {
    if (!session_account) return;
    char *scope = project_ssh_scope();
    bool same = session_account == g_store.client && str_eq(session_scope, scope) && project_ssh_offered();
    free(scope);
    if (!same) servers_ssh_sessions_reset();
}
