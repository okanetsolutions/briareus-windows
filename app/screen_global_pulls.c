#include "screen_global_pulls_internal.h"

bool global_pulls_current(GlobalPullsScreen *s) {
    if (s->client != g_store.client || !store_supports("pulls") || !store_supports("projects")) return false;
    Json *device = device_json(&g_store.device);
    bool same = json_equal(s->device, device); json_free(device); return same;
}
static void clear(GlobalPullsScreen *s) {
    request_cancel(&s->req); global_pulls_free(&s->state);
    free(s->rows); s->rows = NULL; s->row_count = 0;
    s->listed = s->loading = false; s->action_first = s->action_next;
}
static void changed(GlobalPullsScreen *s) {
    // Old document actions must never index a different snapshot.
    s->action_first = s->action_next;
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}
static void board_done(void *owner, Request *req) {
    GlobalPullsScreen *s = owner;
    if (!s->shown || !global_pulls_current(s)) { clear(s); return; }
    if (s->cursor >= s->state.count) return;
    GlobalPullProject *p = &s->state.projects[s->cursor];
    if (!str_ieq(p->project.repo, json_str(json_get(req->args, "repo")))) return;
    if (!req->ok && req->error.status == 400 && !json_is_null(json_get(req->args, "fresh"))) {
        if (screen_pulls_pulls_read(p->project.repo, false, s, board_done, &s->req)) return;
    }
    if (req->ok && global_pulls_adopt(p, req->result)) {
        pulls_note_success(p->project.repo); p->fresh = false;
        p->synced_at = pulls_sync_time(req->result, false, time(NULL));
        // The shared project cache must keep the full server answer, even when this overview caps rows.
        Json *saved = json_clone(req->result); json_set_num(saved, "_receivedAt", (double)time(NULL));
        char *key = xstrfmt("pulls:%s", p->project.repo); cache_store(g_store.cache, saved, key); free(key); json_free(saved);
    } else {
        char *error = request_error_or_unexpected(req); set_string(&p->error, error); free(error);
        ApiError malformed; api_error_init(&malformed); malformed.kind = API_NON_JSON;
        pulls_note_failure(p->project.repo, req->ok ? &malformed : &req->error, time(NULL));
        if (req->error.status == 403 || req->error.status == 404) {
            global_pulls_forget(p);
            char *key = xstrfmt("pulls:%s", p->project.repo); cache_remove(g_store.cache, key); free(key);
        }
    }
    s->cursor++; changed(s); global_pulls_next(s);
}
void global_pulls_next(GlobalPullsScreen *s) {
    if (!s->shown || !g_store.active || !global_pulls_current(s) || s->req) return;
    size_t end = s->limit < s->state.count ? s->limit : s->state.count;
    while (s->cursor < end) {
        GlobalPullProject *p = &s->state.projects[s->cursor];
        if (screen_pulls_pulls_read(p->project.repo, p->fresh, s, board_done, &s->req)) return;
        if (!p->error) set_string(&p->error, "Updates delayed by this repository's cooldown; showing the last snapshot if available.");
        s->cursor++;
    }
    s->loading = false; s->next_poll = time(NULL) + 60;
    poller_finished(&s->poller, false, -1); changed(s);
    if (s->queued_fresh) { s->queued_fresh = false; global_pulls_load(s, true); }
}
static void restore_cached(GlobalPullsScreen *s, size_t first, size_t end) {
    if (end > s->state.count) end = s->state.count;
    for (size_t i = first; i < end; i++) {
        GlobalPullProject *p = &s->state.projects[i];
        if (p->loaded) continue;
        char *key = xstrfmt("pulls:%s", p->project.repo); Json *saved = cache_value(g_store.cache, key); free(key);
        if (saved && global_pulls_adopt(p, saved)) p->synced_at = pulls_sync_time(saved, true, time(NULL));
        json_free(saved);
    }
}
static void projects_done(void *owner, Request *req) {
    GlobalPullsScreen *s = owner;
    if (!s->shown || !global_pulls_current(s)) { clear(s); return; }
    s->reading_projects = false;
    Project *projects = NULL; size_t count = 0;
    if (!req->ok || !projects_parse(req->result, &projects, &count)) {
        char *error = request_error_or_unexpected(req); set_string(&s->error, error); free(error);
        double delay = req->error.retry_after > 60 ? req->error.retry_after : 60;
        if (delay > 2147483647) delay = 2147483647;
        s->projects_retry = time(NULL) + (time_t)delay + 1;
        s->loading = false;
        if (req->error.status == 403 || req->error.status == 404) clear(s);
        poller_finished(&s->poller, false, -1); changed(s); return;
    }
    global_pulls_replace(&s->state, projects, count); projects_free(projects, count);
    cache_store(g_store.cache, req->result, "projects");
    s->listed = true; s->projects_retry = 0; set_string(&s->error, NULL); s->cursor = 0;
    for (size_t i = 0; i < s->state.count; i++) s->state.projects[i].fresh |= s->fresh;
    restore_cached(s, 0, s->limit);
    changed(s); global_pulls_next(s);
}
void global_pulls_load(GlobalPullsScreen *s, bool fresh) {
    if (!s->shown || !g_store.active || !global_pulls_current(s)) return;
    if (s->loading || s->req) { s->queued_fresh |= fresh; return; }
    if (time(NULL) < s->projects_retry) return;
    s->fresh = fresh; s->loading = s->reading_projects = true;
    store_call("projects", json_object(), 0, s, projects_done, 0, &s->req); changed(s);
}
static void visible(Screen *base, bool shown) {
    GlobalPullsScreen *s = (GlobalPullsScreen *)base;
    if (!shown && s->shown) { s->saved_scroll = pane_scroll_y(base->pane); s->restore_scroll = true; }
    s->shown = shown;
    if (shown && global_pulls_current(s)) {
        poller_start(&s->poller, base->pane, GLOBAL_POLL, 1000);
        if (s->reading_projects && !s->req) { s->loading = false; global_pulls_load(s, s->fresh); }
        else if (s->loading && !s->reading_projects && s->listed) global_pulls_next(s);
        else if (!s->listed || time(NULL) >= s->next_poll) { s->loading = false; global_pulls_load(s, false); }
        else poller_finished(&s->poller, false, -1);
    } else {
        poller_stop(&s->poller); request_cancel(&s->req);
        if (!global_pulls_current(s)) clear(s);
    }
}
static void timer(Screen *base, UINT id) {
    GlobalPullsScreen *s = (GlobalPullsScreen *)base;
    if (!global_pulls_current(s)) { clear(s); poller_stop(&s->poller); changed(s); return; }
    if (!poller_fired(&s->poller, id)) return;
    if (s->loading) { global_pulls_next(s); }
    else if (time(NULL) >= s->next_poll) global_pulls_load(s, false);
    poller_finished(&s->poller, false, -1);
}
static void refresh(Screen *base) { global_pulls_load((GlobalPullsScreen *)base, true); }
static void activated(Screen *base, bool active) {
    GlobalPullsScreen *s = (GlobalPullsScreen *)base;
    if (active && s->shown) {
        if (s->loading) global_pulls_next(s);
        else if (time(NULL) >= s->next_poll) global_pulls_load(s, false);
    }
}
Screen *global_pulls_detail(GlobalPullsScreen *s, intptr_t arg) {
    if (!global_pulls_current(s) || arg < s->action_first || arg >= s->action_next) return NULL;
    GlobalPullRow r = s->rows[arg - s->action_first];
    if (r.project >= s->state.count) return NULL;
    GlobalPullProject *p = &s->state.projects[r.project];
    if (r.pull >= p->count) return NULL;
    PullSummary *pull = &p->pulls[r.pull]; StackPosition stack;
    bool stacked = stack_position_parse(json_get(pull->raw, "stack"), json_get(p->board, "stacks"), &stack);
    Screen *detail = pull_detail_screen_new(&p->project, pull->number, stacked ? &stack : NULL, pull);
    if (stacked) stack_position_free(&stack);
    set_string(&s->selected_repo, p->project.repo); s->selected_number = pull->number;
    return detail;
}
static void action(Screen *base, int id, intptr_t arg, POINT pt) {
    GlobalPullsScreen *s = (GlobalPullsScreen *)base;
    if (!global_pulls_current(s)) return;
    if (id == GLOBAL_OPEN) { Screen *detail = global_pulls_detail(s, arg); if (detail) app_push_from(base, detail); return; }
    if (id == GLOBAL_REFRESH) { refresh(base); return; }
    if (id == GLOBAL_MORE_ROWS) { s->visible_rows += 100; changed(s); return; }
    if (id == GLOBAL_MORE) {
        if (s->loading || s->req || s->limit >= s->state.count) return;
        s->cursor = s->limit; s->limit += GLOBAL_PULLS_PROJECT_BATCH; s->loading = true;
        restore_cached(s, s->cursor, s->limit); changed(s); global_pulls_next(s); return;
    }
    if (id >= GLOBAL_FILTER_BASE && id < GLOBAL_FILTER_BASE + FILTER_KIND_COUNT) { global_pulls_pick(s, (FilterKind)(id - GLOBAL_FILTER_BASE), pt); return; }
    if (id == GLOBAL_STACK) s->hide_stacked = !s->hide_stacked;
    else if (id == GLOBAL_CHECKS) s->filter.hide_failing_checks = !s->filter.hide_failing_checks;
    else if (id == GLOBAL_CLEAR) { board_filter_free(&s->filter); board_filter_init(&s->filter); s->hide_stacked = false; }
    else return;
    global_pulls_filters_save(s); changed(s);
}
static void destroy(Screen *base) {
    GlobalPullsScreen *s = (GlobalPullsScreen *)base; poller_stop(&s->poller); clear(s);
    api_client_release(s->client); json_free(s->device); board_filter_free(&s->filter); free(s->error); free(s->selected_repo); screen_release(base);
}
static void place(Screen *base, const RECT *content, int scroll_y) {
    (void)content; (void)scroll_y; GlobalPullsScreen *s = (GlobalPullsScreen *)base;
    if (s->shown && s->restore_scroll) {
        s->restore_scroll = false;
        pane_scroll_to(base->pane, s->saved_scroll + px(8));
    }
}
static const ScreenVTable VT = {.destroy = destroy, .layout = global_pulls_layout, .header = global_pulls_header,
    .action = action, .visible = visible, .refresh = refresh, .activated = activated, .timer = timer,
    .place = place, .effective_visibility = true, .detachable = true};
Screen *global_pulls_screen_new(void) {
    GlobalPullsScreen *s = xcalloc(1, sizeof *s); s->base.vt = &VT;
    s->client = api_client_retain(g_store.client); s->device = device_json(&g_store.device);
    char *scope = json_serialize(s->device, true); s->base.id = xstrfmt("global-pulls:%p:%s", (void *)s->client, scope); free(scope);
    s->visible_rows = 100; s->limit = GLOBAL_PULLS_PROJECT_BATCH; board_filter_init(&s->filter);
    Json *saved = cache_value(g_store.cache, "global-pulls-filters");
    for (int k = 0; k < FILTER_KIND_COUNT; k++) board_filter_set(&s->filter, (FilterKind)k, json_str(json_get(saved, filter_kind_name((FilterKind)k))));
    s->filter.hide_failing_checks = json_bool_is(json_get(saved, "hideFailingChecks"), true);
    s->hide_stacked = json_bool_is(json_get(saved, "hideLaterStacked"), true); json_free(saved);
    // Cached scope is provisional until /projects confirms it; errors must never call it complete.
    Json *catalog = cache_value(g_store.cache, "projects"); Project *projects = NULL; size_t count = 0;
    if (global_pulls_current(s) && projects_parse(catalog, &projects, &count)) {
        global_pulls_replace(&s->state, projects, count); restore_cached(s, 0, s->limit);
    }
    projects_free(projects, count); json_free(catalog);
    return &s->base;
}
