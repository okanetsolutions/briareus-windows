#include "action_runtime.h"
#include "screens.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

void action_runtime_init(ActionRuntime *s, const char *repo, const char *binary, void (*changed)(void *), void *ctx) {
    memset(s, 0, sizeof *s);
    s->client = g_store.client; if (s->client) api_client_retain(s->client);
    s->device = xstrdup(g_store.device.id); s->repo = xstrdup(repo); s->binary = xstrdup(binary);
    s->changed = changed; s->ctx = ctx;
}
bool action_runtime_current(const ActionRuntime *s) {
    if (s->closed || !s->client || s->client != g_store.client || !str_eq(s->device, g_store.device.id)) return false;
    if (!store_can_manage() || !store_supports("action")) return false;
    if (permission_rank(g_store.device.permission) == 2) return true;
    for (size_t i = 0; i < g_store.device.repo_count; i++) if (str_eq(s->repo, g_store.device.repos[i])) return true;
    return false;
}
static void changed(ActionRuntime *s) { if (s->changed) s->changed(s->ctx); }
static void catalog_ready(ActionRuntime *s, const Json *providers) {
    if (!runtime_catalog_for_binary(s->raw, providers, s->binary, &s->catalog)) {
        set_string(&s->error, "The server returned an invalid runtime catalog."); changed(s); return;
    }
    if (s->catalog.has_default) runtime_choice_copy(&s->choice, &s->catalog.def);
    else runtime_catalog_first_available(&s->catalog, &s->choice);
    s->ready = s->choice.provider_id != 0;
    if (!s->ready) set_string(&s->error, "No available provider with a verified matching CLI and model. Check the server's provider configuration and login.");
    changed(s);
}
static void providers_done(void *owner, Request *req) {
    ActionRuntime *s = owner;
    if (!action_runtime_current(s)) { set_string(&s->error, "Connection or permission changed. Close and reopen this action."); changed(s); return; }
    if (!req->ok) { request_error_into(&s->error, req); changed(s); return; }
    catalog_ready(s, req->result);
}
static void runtimes_done(void *owner, Request *req) {
    ActionRuntime *s = owner;
    if (!action_runtime_current(s)) { set_string(&s->error, "Connection or permission changed. Close and reopen this action."); changed(s); return; }
    if (!req->ok) { request_error_into(&s->error, req); changed(s); return; }
    s->raw = json_clone(req->result);
    const Json *providers = json_get(s->raw, "providers"); bool missing = false;
    for (size_t i = 0; i < json_count(providers); i++)
        if (str_empty(json_str(json_get(json_at(providers, i), "binary")))) missing = true;
    if (!missing) { catalog_ready(s, NULL); return; }
    if (!store_supports("providers")) {
        set_string(&s->error, "This server omits CLI identity from runtimes. Verifying it requires access to GET /providers (admin on current servers). No action was started.");
        changed(s); return;
    }
    store_call("providers", json_object(), 0, s, providers_done, 0, &s->read);
}
void action_runtime_load(ActionRuntime *s) {
    if (s->read || s->raw || s->closed) return;
    if (!action_runtime_current(s) || !store_supports("runtimes")) {
        set_string(&s->error, "This action requires manage access to this project and a supported runtime catalog."); changed(s); return;
    }
    Json *args = json_object(); json_set_str(args, "repo", s->repo);
    store_call("runtimes", args, 0, s, runtimes_done, 0, &s->read);
}
bool action_runtime_select(ActionRuntime *s, int provider, const char *model, const char *effort) {
    if (!s->ready || !action_runtime_current(s)) return false;
    RuntimeChoice proposed = {provider, (char *)model, (char *)effort}, valid;
    if (!runtime_catalog_offered(&s->catalog, &proposed, &valid)) return false;
    if (effort && !str_eq(effort, valid.effort)) { runtime_choice_free(&valid); return false; }
    runtime_choice_free(&s->choice); s->choice = valid; return true;
}
bool action_runtime_take(ActionRuntime *s, RuntimeChoice *out) {
    if (!s->ready || !action_runtime_current(s)) return false;
    return runtime_catalog_offered(&s->catalog, &s->choice, out);
}
void action_runtime_free(ActionRuntime *s) {
    s->closed = true; request_cancel(&s->read);
    runtime_catalog_free(&s->catalog); runtime_choice_free(&s->choice); json_free(s->raw);
    if (s->client) api_client_release(s->client);
    free(s->device); free(s->repo); free(s->binary); free(s->error);
}
