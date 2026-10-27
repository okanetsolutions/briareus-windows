#include "screen_resources_internal.h"
#include <math.h>

static ResourcesScreen *screens;

bool resources_current(ResourcesScreen *s) {
    if (s->client != g_store.client || !store_supports(resources_operation(s))) return false;
    Json *device = device_json(&g_store.device);
    bool same = json_equal(device, s->device); json_free(device);
    return same;
}
double resources_retry_until(ResourcesScreen *s) { return json_num_or(json_get(json_get(g_store.resources_retries, s->retry_key), "until"), 0); }
static void schedule(ResourcesScreen *s) {
    double remaining = resources_retry_until(s) - (double)time(NULL);
    // Keep Poller's backoff arithmetic away from a very large Retry-After value.
    int delay = remaining > 0 ? (int)fmin(2147483000.0, ceil(remaining * 1000)) : stats_poll_ms(s->loaded ? &s->reading.host : NULL);
    poller_set_base(&s->poller, delay);
    poller_finished(&s->poller, false, -1);
}
static double refusal_epoch(const char *key) {
    return json_num_or(json_get(json_get(g_store.resources_retries, key), "epoch"), 0);
}
void resources_clear_figures(ResourcesScreen *s) {
    if (s->dashboard) s->dashboard->action_count = 0;
    session_stats_free(&s->reading); server_stats_free(&s->server); open_stats_free(&s->sessions); s->loaded = s->fresh = false;
    if (s->base.pane) pane_relayout(s->base.pane);
}
void resources_discard_revoked(ResourcesScreen *s) {
    double epoch = refusal_epoch(s->retry_key);
    if (s->reading_epoch == epoch) return;
    resources_clear_figures(s); s->reading_epoch = epoch;
}
static void record_failure(const char *key, Request *req) {
    bool refused = req->error.status == 403 || req->error.status == 404;
    const Json *entry = json_get(g_store.resources_retries, key);
    int failures = json_int_or(json_get(entry, "failures"), 0);
    if (failures < 6) failures++;
    double epoch = refusal_epoch(key) + (refused ? 1 : 0);
    double previous = json_num_or(json_get(entry, "until"), 0);
    double retry = req->error.retry_after;
    if (req->error.status == 429 && retry < 0) retry = 60;
    double until = stats_retry_deadline((double)time(NULL), previous, failures, retry);
    Json *cooldown = json_object(); json_set_num(cooldown, "until", until); json_set_num(cooldown, "failures", failures);
    json_set_num(cooldown, "epoch", epoch);
    if (!g_store.resources_retries) g_store.resources_retries = json_object();
    json_object_set(g_store.resources_retries, key, cooldown);
    if (!refused) return;
    // Registered live views share the refusal, including hidden and detached views.
    for (ResourcesScreen *s = screens; s; s = s->next) {
        if (s->client != req->client || !str_eq(s->retry_key, key)) continue;
        resources_discard_revoked(s);
        request_error_into(&s->error, req);
    }
}
typedef struct { Json *device; char *key; double epoch; } ReadCompletion;
static void read_completed(void *ctx, Request *req) {
    ReadCompletion *completion = ctx;
    // The metadata belongs to the request, never to its possibly destroyed UI owner.
    req->auth_checked = true;
    bool current = req->client == g_store.client && g_store.has_device;
    Json *device = current ? device_json(&g_store.device) : NULL;
    current = current && json_equal(device, completion->device);
    json_free(device);
    if (current) {
        if (!req->ok && api_error_unauthorized(&req->error)) {
            // A token refusal applies to this connection; project refusals stay session-scoped.
            for (ResourcesScreen *s = screens; s; s = s->next) {
                if (s->client != req->client) continue;
                resources_clear_figures(s); poller_stop(&s->poller); request_cancel(&s->read);
            }
            store_invalidate_credentials(&req->error);
        } else if (req->cancelled && !req->ok) record_failure(completion->key, req);
        else if (req->ok) req->arg = completion->epoch != refusal_epoch(completion->key);
    }
    json_free(completion->device); free(completion->key); free(completion);
    req->completion_ctx = NULL;
}
static void read_done(void *owner, Request *req) {
    ResourcesScreen *s = owner;
    if (!s->shown || !resources_current(s)) return;
    resources_discard_revoked(s);
    // The finalizer compares the request's start epoch before releasing its metadata.
    if (req->arg) { schedule(s); pane_relayout(s->base.pane); return; }
    bool ok = req->ok && resources_adopt(s, req->result);
    if (ok) {
        s->loaded = s->fresh = true; s->reading_epoch = refusal_epoch(s->retry_key); s->received_at = (double)time(NULL);
        set_string(&s->error, NULL);
        // A success already in flight cannot erase another screen's newer Retry-After.
        Json *cooldown = (Json *)json_get(g_store.resources_retries, s->retry_key);
        if (resources_retry_until(s) > (double)time(NULL)) json_set_num(cooldown, "failures", 0);
        else {
            // Keep the epoch after recovery so older in-flight successes remain obsolete.
            json_object_remove(cooldown, "until"); json_object_remove(cooldown, "failures");
        }
    } else {
        s->fresh = false;
        char *error = request_error_or_unexpected(req); set_string(&s->error, error); free(error);
        record_failure(s->retry_key, req);
    }
    schedule(s); pane_relayout(s->base.pane);
}
static void load(ResourcesScreen *s) {
    if (!s->shown || !g_store.active || !resources_current(s) || s->read) return;
    resources_discard_revoked(s);
    if (resources_retry_until(s) > (double)time(NULL)) { schedule(s); return; }
    Json *args = json_object();
    if (s->kind == RESOURCE_SESSION) json_set_str(args, "sessionId", session_id(&s->session));
    Request *req = store_call(resources_operation(s), args, 0, s, read_done, 0, &s->read);
    ReadCompletion *completion = xcalloc(1, sizeof *completion);
    completion->device = json_clone(s->device); completion->key = xstrdup(s->retry_key);
    completion->epoch = refusal_epoch(s->retry_key);
    req->completion_ctx = completion; req->completed = read_completed;
}
static void visible(Screen *base, bool shown) {
    ResourcesScreen *s = (ResourcesScreen *)base; s->shown = shown;
    if (s->list) { s->list->base.pane = base->pane; visible(&s->list->base, shown && resources_current(s)); }
    if (!shown || !resources_current(s)) {
        poller_stop(&s->poller); request_cancel(&s->read); s->fresh = false; return;
    }
    if (!s->poller.running) poller_start(&s->poller, base->pane, s->kind == RESOURCE_LIST ? RESOURCES_POLL + 1 : RESOURCES_POLL, stats_poll_ms(s->loaded ? &s->reading.host : NULL));
    load(s);
}
static void timer(Screen *base, UINT id) {
    ResourcesScreen *s = (ResourcesScreen *)base;
    if (!resources_current(s)) { visible(base, false); pane_relayout(base->pane); return; }
    if (s->list) timer(&s->list->base, id);
    if (poller_fired(&s->poller, id)) load(s);
}
static void refresh(Screen *base) {
    ResourcesScreen *s = (ResourcesScreen *)base;
    if (!s->shown) return;
    // A refresh coalesces with an outstanding read, and cannot bypass the shared deadline.
    if (!resources_current(s)) { visible(base, false); return; }
    if (s->list) refresh(&s->list->base);
    load(s); pane_relayout(base->pane);
}
static void activated(Screen *base, bool active) { if (active) refresh(base); }
static void header(Screen *base, HeaderInfo *info) {
    ResourcesScreen *s = (ResourcesScreen *)base;
    snprintf(info->title, sizeof info->title, "%s", s->kind == RESOURCE_SERVER ? "Server" : "Resources");
    if (!resources_current(s)) return;
    if (s->kind == RESOURCE_SESSION) snprintf(info->subtitle, sizeof info->subtitle, "%s · %s", session_display_title(&s->session), session_id(&s->session));
    info->buttons[info->button_count++] = (HeaderButton){.glyph = 0xE72C, .action = RESOURCES_REFRESH, .enabled = true, .tip = "Read the figures again"};
}
static void action(Screen *base, int id, intptr_t arg, POINT pt) {
    (void)pt; ResourcesScreen *s = (ResourcesScreen *)base;
    if (id == RESOURCES_OPEN && resources_current(s) && s->list && store_supports("session_stats")) {
        ResourcesScreen *list = s->list; resources_discard_revoked(list);
        if (!resources_current(list) || !list->loaded || arg < s->action_first) return;
        size_t index = (size_t)(arg - s->action_first);
        if (index >= s->action_count || index >= list->sessions.count) return;
        app_push_from(base, resources_screen_new(&list->sessions.items[index].record)); return;
    }
    if (id == RESOURCES_REFRESH) refresh(base);
}
static void destroy(Screen *base) {
    ResourcesScreen *s = (ResourcesScreen *)base; visible(base, false);
    ResourcesScreen **slot = &screens;
    while (*slot && *slot != s) slot = &(*slot)->next;
    if (*slot) *slot = s->next;
    if (s->list) destroy(&s->list->base);
    session_free(&s->session); resources_clear_figures(s); json_free(s->device);
    free(s->retry_key); free(s->error); api_client_release(s->client); screen_release(base);
}
static const ScreenVTable VT = {
    .destroy = destroy, .layout = resources_layout, .header = header, .action = action,
    .visible = visible, .timer = timer, .refresh = refresh, .activated = activated, .detachable = true, .effective_visibility = true,
};
static ResourcesScreen *new_reading(ResourceKind kind, const Session *session) {
    ResourcesScreen *s = xcalloc(1, sizeof *s); s->base.vt = &VT; s->kind = kind;
    s->client = api_client_retain(g_store.client); s->device = device_json(&g_store.device);
    char *scope = json_serialize(s->device, true);
    if (session) {
        s->retry_key = xstrfmt("%s:session_stats:%s", scope, session_id(session));
        session_copy(&s->session, session);
    } else s->retry_key = xstrfmt("%s:%s", scope, resources_operation(s));
    free(scope);
    s->base.id = xstrfmt("resources:%p:%s:%s", (void *)s->client, g_store.server ? g_store.server : "", s->retry_key);
    s->next = screens; screens = s;
    return s;
}
Screen *resources_screen_new(const Session *session) { return &new_reading(RESOURCE_SESSION, session)->base; }
Screen *server_resources_screen_new(void) {
    ResourcesScreen *s = new_reading(RESOURCE_SERVER, NULL);
    s->list = new_reading(RESOURCE_LIST, NULL); s->list->dashboard = s;
    return &s->base;
}
