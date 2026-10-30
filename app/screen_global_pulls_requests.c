#include "screen_global_pulls_internal.h"

// Live controllers are UI-thread owned; completions hold only a device snapshot, never a screen pointer.
static GlobalPullsScreen *live_screens;
static const char PROJECTS_KEY[] = "@global-project-catalog";
void global_pulls_register(GlobalPullsScreen *s) { s->next_live = live_screens; live_screens = s; }
void global_pulls_unregister(GlobalPullsScreen *s) {
    for (GlobalPullsScreen **slot = &live_screens; *slot; slot = &(*slot)->next_live) {
        if (*slot == s) { *slot = s->next_live; return; }
    }
}
time_t global_pulls_projects_deadline(void) { return pulls_retry_deadline(PROJECTS_KEY); }
time_t global_pulls_projects_failed(const ApiError *error) {
    ApiError scoped = *error;
    if (scoped.retry_after < 60) scoped.retry_after = 60;
    pulls_note_failure(PROJECTS_KEY, &scoped, time(NULL)); return global_pulls_projects_deadline();
}
void global_pulls_projects_succeeded(void) { pulls_note_success(PROJECTS_KEY); }
static bool completion_current(const Json *device, const Request *req) {
    if (req->client != g_store.client || !store_supports(req->operation)) return false;
    Json *now = device_json(&g_store.device); bool current = json_equal(device, now); json_free(now); return current;
}
static void clear_refused_cache(const char *repo) {
    char *key = xstrfmt("pulls:%s", repo); cache_remove(g_store.cache, key); free(key);
}
static void completed(void *context, Request *req) {
    Json *device = context;
    bool current = completion_current(device, req); json_free(device);
    req->auth_checked = true;
    if (!current || req->ok) return;
    if (api_error_unauthorized(&req->error)) { store_invalidate_credentials(&req->error); return; }
    bool refused = req->error.status == 403 || req->error.status == 404;
    // Normal failures go through the controller once; cancelled failures still establish a shared deadline.
    if (!req->cancelled && !refused) return;
    bool projects = str_eq(req->operation, "projects");
    const char *repo = json_str(json_get(req->args, "repo"));
    if (!projects && str_empty(repo)) return;
    if (req->cancelled) {
        if (projects) global_pulls_projects_failed(&req->error);
        else pulls_note_failure(repo, &req->error, time(NULL));
    }
    if (refused) {
        if (projects) cache_remove(g_store.cache, "projects");
        else clear_refused_cache(repo);
    }
    // Another window can have reopened this scope before the cancelled answer arrived.
    for (GlobalPullsScreen *s = live_screens; s; s = s->next_live) {
        if (!global_pulls_current(s)) continue;
        if (projects) {
            request_error_into(&s->error, req); s->projects_retry = global_pulls_projects_deadline();
            if (refused) { global_pulls_free(&s->state); s->listed = false; }
        } else {
            for (size_t i = 0; i < s->state.count; i++) {
                GlobalPullProject *p = &s->state.projects[i];
                if (!str_ieq(p->project.repo, repo)) continue;
                if (refused) global_pulls_forget(p);
                request_error_into(&p->error, req);
            }
        }
        global_pulls_changed(s);
    }
}
void global_pulls_track(GlobalPullsScreen *s) {
    if (!s->req) return;
    s->req->completion_ctx = json_clone(s->device); s->req->completed = completed;
}
