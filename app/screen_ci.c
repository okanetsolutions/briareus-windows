#include "screen_ci_internal.h"
#include "dialogs.h"
#include <string.h>

bool ci_current(CIScreen *s) {
    if (s->client != g_store.client || !store_supports("ci_stats")) return false;
    Json *device = device_json(&g_store.device);
    bool same = json_equal(device, s->device); json_free(device); return same;
}
static void clear(CIScreen *s) {
    request_cancel(&s->reads[0]); request_cancel(&s->reads[1]);
    json_free(s->stats); s->stats = NULL; json_free(s->runs); s->runs = NULL;
    s->offset = -1; s->truncated = s->capped = false; s->action_first = s->action_next;
}
static void done(void *owner, Request *req) {
    CIScreen *s = owner; int channel = req->tag;
    if (!s->shown || !ci_current(s)) return;
    if (channel && !store_supports("ci_runs")) return;
    Json *parsed = req->ok ? (channel ? ci_page_parse(req->result, s->requested_offset) : ci_stats_parse(req->result)) : NULL;
    if (!parsed) {
        char *error = request_error_or_unexpected(req); set_string(&s->errors[channel], error); free(error);
        if (s->failures[channel] < 6) s->failures[channel]++;
        s->retry[channel] = stats_retry_deadline((double)time(NULL), s->retry[channel], s->failures[channel], req->error.retry_after);
        // Never leave accessible figures behind a permission refusal.
        if (req->error.status == 401 || req->error.status == 403 || req->error.status == 404) {
            if (channel) { json_free(s->runs); s->runs = NULL; s->offset = -1; }
            else { json_free(s->stats); s->stats = NULL; }
        }
    } else {
        set_string(&s->errors[channel], NULL); s->failures[channel] = 0;
        if (!channel) { json_free(s->stats); s->stats = parsed; parsed = NULL; }
        else {
            if (!s->requested_offset) { json_free(s->runs); s->runs = json_array(); }
            const Json *rows = json_get(parsed, "runs");
            for (size_t i = 0; i < json_count(rows); i++) json_array_push(s->runs, json_clone(json_at(rows, i)));
            s->offset = json_int_or(json_get(parsed, "nextOffset"), -1);
            s->truncated = json_bool_is(json_get(parsed, "truncated"), true);
            s->capped = json_count(s->runs) >= 1000 && s->offset >= 0;
            if (s->capped) s->offset = -1;
        }
    }
    json_free(parsed); s->action_first = s->action_next; pane_relayout(s->base.pane);
}
static void read_channel(CIScreen *s, int channel, int offset) {
    if (s->reads[channel] || s->retry[channel] > (double)time(NULL)) return;
    if (channel && !store_supports("ci_runs")) return;
    Json *args = channel ? ci_runs_query(s->window, offset) : json_clone(s->window);
    if (channel) s->requested_offset = offset;
    store_call(channel ? "ci_runs" : "ci_stats", args, 0, s, done, channel, &s->reads[channel]);
}
void ci_load(CIScreen *s, bool reset) {
    if (!s->shown || !g_store.active || !ci_current(s)) return;
    if (!reset && (s->reads[0] || s->reads[1])) return;
    if (reset) clear(s);
    // A refresh starts a new window only when both channels can read it.
    if (s->retry[0] > (double)time(NULL) || s->retry[1] > (double)time(NULL)) return;
    json_free(s->window); s->window = ci_query(s->days, (double)time(NULL) * 1000, s->statuses, s->repo, s->branch);
    clear(s); read_channel(s, 0, 0); read_channel(s, 1, 0);
}
static void visible(Screen *base, bool shown) {
    CIScreen *s = (CIScreen *)base; s->shown = shown;
    if (shown && ci_current(s)) {
        poller_start(&s->poller, base->pane, CI_POLL, 60000);
        if (!s->stats && !s->runs) ci_load(s, false);
    } else {
        poller_stop(&s->poller); request_cancel(&s->reads[0]); request_cancel(&s->reads[1]);
        if (!ci_current(s)) clear(s);
    }
}
static void refresh(Screen *base) { CIScreen *s = (CIScreen *)base; ci_load(s, false); pane_relayout(base->pane); }
static void timer(Screen *base, UINT id) {
    CIScreen *s = (CIScreen *)base;
    if (!ci_current(s)) { visible(base, false); pane_relayout(base->pane); return; }
    if (!store_supports("ci_runs")) { request_cancel(&s->reads[1]); json_free(s->runs); s->runs = NULL; s->offset = -1; }
    if (poller_fired(&s->poller, id)) {
        // Keep the browsed run snapshot and its pinned window until an explicit refresh.
        if (s->window) read_channel(s, 0, 0); else ci_load(s, false);
        poller_finished(&s->poller, false, -1);
    }
}
static void header(Screen *base, HeaderInfo *info) {
    CIScreen *s = (CIScreen *)base; snprintf(info->title, sizeof info->title, "CI/CD");
    snprintf(info->subtitle, sizeof info->subtitle, "%s · last %d days", s->repo && *s->repo ? s->repo : "All accessible projects", s->days);
    if (ci_current(s)) info->buttons[info->button_count++] = (HeaderButton){.glyph = 0xE72C, .action = CI_REFRESH, .enabled = true, .tip = "Refresh CI history"};
}
static void action(Screen *base, int id, intptr_t arg, POINT pt) {
    (void)pt; CIScreen *s = (CIScreen *)base;
    if (!ci_current(s)) return;
    if (id == CI_REFRESH) { refresh(base); return; }
    if (id == CI_MORE) {
        if (s->shown && g_store.active && s->offset >= 0 && !s->reads[0]) read_channel(s, 1, s->offset);
        return;
    }
    if (id == CI_OPEN) {
        if (!store_supports("ci_runs") || arg < s->action_first || arg >= s->action_next) return;
        const Json *run = json_at(s->runs, (size_t)(arg - s->action_first));
        open_web_url(json_str(json_get(run, "url"))); return;
    }
    if (id == CI_DAYS) { if (arg != 7 && arg != 30 && arg != 90) return; s->days = (int)arg; }
    else if (id == CI_KIND) s->statuses = arg != 0;
    else if (id == CI_REPO || id == CI_BRANCH) {
        char **slot = id == CI_REPO ? &s->repo : &s->branch;
        char *text = dialog_text(app_dialog_owner(), "CI/CD filter", id == CI_REPO ? "Repository owner/name (empty for all accessible projects)" : "Exact branch (empty for all)", "Apply", *slot ? *slot : "");
        if (!text) return;
        char *trimmed = str_trim(text); free(text); free(*slot); *slot = trimmed;
        if (!ci_current(s)) return;
    } else return;
    ci_load(s, true); pane_relayout(base->pane); pane_header_changed(base->pane);
}
static void activated(Screen *base, bool active) {
    CIScreen *s = (CIScreen *)base;
    if (active && s->shown && ci_current(s) && !s->stats && !s->runs) ci_load(s, false);
}
static void destroy(Screen *base) {
    CIScreen *s = (CIScreen *)base; visible(base, false); clear(s);
    json_free(s->device); json_free(s->window); free(s->repo); free(s->branch);
    free(s->errors[0]); free(s->errors[1]); api_client_release(s->client); screen_release(base);
}
static const ScreenVTable VT = {.destroy = destroy, .layout = ci_layout, .header = header, .action = action,
    .visible = visible, .timer = timer, .refresh = refresh, .activated = activated, .detachable = true, .effective_visibility = true};
Screen *ci_screen_new(void) {
    CIScreen *s = xcalloc(1, sizeof *s); s->base.vt = &VT;
    s->client = api_client_retain(g_store.client); s->device = device_json(&g_store.device); s->days = 30; s->offset = -1;
    char *scope = json_serialize(s->device, true);
    s->base.id = xstrfmt("ci-history:%p:%s", (void *)s->client, scope); free(scope);
    return &s->base;
}
