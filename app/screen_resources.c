#include "screen_resources_internal.h"
#include <math.h>

bool resources_current(ResourcesScreen *s) {
    if (s->client != g_store.client || !store_supports("session_stats")) return false;
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
static void read_done(void *owner, Request *req) {
    ResourcesScreen *s = owner;
    if (!s->shown || !resources_current(s)) return;
    SessionStats next;
    bool ok = req->ok && session_stats_parse(req->result, session_id(&s->session), &next);
    if (ok) {
        session_stats_free(&s->reading); s->reading = next;
        s->loaded = s->fresh = true; s->received_at = (double)time(NULL);
        set_string(&s->error, NULL);
        json_object_remove(g_store.resources_retries, s->retry_key);
    } else {
        s->fresh = false;
        // A project permission refusal must not keep private figures on screen.
        if (req->error.status == 403 || req->error.status == 404) { session_stats_free(&s->reading); s->loaded = false; }
        char *error = request_error_or_unexpected(req); set_string(&s->error, error); free(error);
        const Json *entry = json_get(g_store.resources_retries, s->retry_key);
        int failures = json_int_or(json_get(entry, "failures"), 0);
        if (failures < 6) failures++;
        double previous = json_num_or(json_get(entry, "until"), 0);
        double retry = req->error.retry_after;
        if (req->error.status == 429 && retry < 0) retry = 60;
        double until = stats_retry_deadline((double)time(NULL), previous, failures, retry);
        Json *cooldown = json_object(); json_set_num(cooldown, "until", until); json_set_num(cooldown, "failures", failures);
        if (!g_store.resources_retries) g_store.resources_retries = json_object();
        json_object_set(g_store.resources_retries, s->retry_key, cooldown);
    }
    schedule(s); pane_relayout(s->base.pane);
}
static void load(ResourcesScreen *s) {
    if (!s->shown || !g_store.active || !resources_current(s) || s->read) return;
    if (resources_retry_until(s) > (double)time(NULL)) { schedule(s); return; }
    Json *args = json_object(); json_set_str(args, "sessionId", session_id(&s->session));
    store_call("session_stats", args, 0, s, read_done, 0, &s->read);
}
static void visible(Screen *base, bool shown) {
    ResourcesScreen *s = (ResourcesScreen *)base; s->shown = shown;
    if (!shown || !resources_current(s)) {
        poller_stop(&s->poller); request_cancel(&s->read); s->fresh = false; return;
    }
    if (!s->poller.running) poller_start(&s->poller, base->pane, RESOURCES_POLL, stats_poll_ms(s->loaded ? &s->reading.host : NULL));
    load(s);
}
static void timer(Screen *base, UINT id) {
    ResourcesScreen *s = (ResourcesScreen *)base;
    if (!resources_current(s)) { visible(base, false); pane_relayout(base->pane); return; }
    if (poller_fired(&s->poller, id)) load(s);
}
static void refresh(Screen *base) {
    ResourcesScreen *s = (ResourcesScreen *)base;
    // A refresh coalesces with an outstanding read, and cannot bypass the shared deadline.
    if (!resources_current(s)) { visible(base, false); return; }
    load(s); pane_relayout(base->pane);
}
static void activated(Screen *base, bool active) { if (active) refresh(base); }
static void header(Screen *base, HeaderInfo *info) {
    ResourcesScreen *s = (ResourcesScreen *)base;
    snprintf(info->title, sizeof info->title, "Resources");
    if (!resources_current(s)) return;
    snprintf(info->subtitle, sizeof info->subtitle, "%s · %s", session_display_title(&s->session), session_id(&s->session));
    info->buttons[info->button_count++] = (HeaderButton){.glyph = 0xE72C, .action = RESOURCES_REFRESH, .enabled = true, .tip = "Read the figures again"};
}
static void action(Screen *base, int id, intptr_t arg, POINT pt) {
    (void)arg; (void)pt;
    if (id == RESOURCES_REFRESH) refresh(base);
}
static void destroy(Screen *base) {
    ResourcesScreen *s = (ResourcesScreen *)base; visible(base, false);
    session_free(&s->session); session_stats_free(&s->reading); json_free(s->device);
    free(s->retry_key); free(s->error); api_client_release(s->client); screen_release(base);
}
static const ScreenVTable VT = {
    .destroy = destroy, .layout = resources_layout, .header = header, .action = action,
    .visible = visible, .timer = timer, .refresh = refresh, .activated = activated, .detachable = true,
};
Screen *resources_screen_new(const Session *session) {
    ResourcesScreen *s = xcalloc(1, sizeof *s); s->base.vt = &VT;
    s->client = api_client_retain(g_store.client); s->device = device_json(&g_store.device);
    char *scope = json_serialize(s->device, true);
    s->retry_key = xstrfmt("%s:%s", scope, session_id(session)); free(scope);
    s->base.id = xstrfmt("resources:%s:%s", g_store.server ? g_store.server : "", s->retry_key);
    session_copy(&s->session, session);
    return &s->base;
}
