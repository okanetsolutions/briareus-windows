#include "screen_security_internal.h"
#include "board.h"

bool security_current(SecurityScreen *s) {
    if (s->client != g_store.client || !store_supports("code_review_rounds") || !store_supports("pulls")) return false;
    Json *device = device_json(&g_store.device);
    bool same = json_equal(device, s->device); json_free(device); return same;
}
size_t security_read_count(const SecurityScreen *s) {
    size_t n = 0;
    for (size_t i = 0; i < s->pull_count; i++) n += s->pulls[i].read;
    return n;
}
bool security_done(const SecurityScreen *s) {
    return s->started && !s->read && !s->error && s->next_project >= s->project_count && s->next_pull >= s->pull_count;
}
static void pull_free(SecurityPull *p) {
    project_free(&p->project); free(p->title); free(p->error);
    if (p->has_outcome) review_outcome_free(&p->outcome);
}
static void clear(SecurityScreen *s) {
    request_cancel(&s->read);
    for (size_t i = 0; i < s->pull_count; i++) pull_free(&s->pulls[i]);
    free(s->pulls); s->pulls = NULL; s->pull_count = s->next_pull = 0;
    projects_free(s->projects, s->project_count); s->projects = NULL; s->project_count = s->next_project = 0;
    free(s->error); s->error = NULL; free(s->skipped); s->skipped = NULL;
    s->started = s->capped = false; s->generation++;
}
static void step(SecurityScreen *s);
static void outcome_done(void *owner, Request *req) {
    SecurityScreen *s = owner;
    if (!security_current(s) || s->next_pull >= s->pull_count) return;
    SecurityPull *p = &s->pulls[s->next_pull++];
    p->read = true;
    Json *state = req->ok ? review_outcome_state(req->result, REVIEW_OUTCOME_FROM_READ, NULL) : NULL;
    p->has_outcome = state && review_outcome_parse(state, &p->outcome);
    if (!p->has_outcome) review_outcome_free(&p->outcome);
    if (!req->ok) p->error = request_error_or_unexpected(req);
    json_free(state);
    step(s); pane_relayout(s->base.pane);
}
static void add_pulls(SecurityScreen *s, const Project *project, const Json *rows) {
    size_t count = 0;
    PullSummary *pulls = pull_summaries_parse(rows, &count);
    for (size_t i = 0; i < count; i++) {
        if (s->pull_count >= SECURITY_PULL_LIMIT) { s->capped = true; break; }
        s->pulls = xrealloc(s->pulls, (s->pull_count + 1) * sizeof *s->pulls);
        SecurityPull *p = &s->pulls[s->pull_count++]; memset(p, 0, sizeof *p);
        project_copy(&p->project, project); p->number = pulls[i].number; p->title = xstrdup(pulls[i].title ? pulls[i].title : "");
    }
    pull_summaries_free(pulls, count);
}
static void pulls_done(void *owner, Request *req) {
    SecurityScreen *s = owner;
    if (!security_current(s) || s->next_project >= s->project_count) return;
    const Project *project = &s->projects[s->next_project++];
    if (req->ok && json_is_array(json_get(req->result, "pulls"))) add_pulls(s, project, json_get(req->result, "pulls"));
    else {
        char *skipped = s->skipped ? xstrfmt("%s, %s", s->skipped, project->repo) : xstrdup(project->repo);
        free(s->skipped); s->skipped = skipped;
    }
    step(s); pane_relayout(s->base.pane);
}
static void projects_done(void *owner, Request *req) {
    SecurityScreen *s = owner;
    if (!security_current(s)) return;
    if (!req->ok || !projects_parse(req->result, &s->projects, &s->project_count)) {
        char *error = request_error_or_unexpected(req); set_string(&s->error, error); free(error);
    }
    step(s); pane_relayout(s->base.pane);
}
// The next read the scan owes, while the screen shows: a project's pull requests first, then each one's outcome.
static void step(SecurityScreen *s) {
    if (!s->shown || s->read || s->error || !g_store.active || !security_current(s)) return;
    Json *args = json_object();
    if (!s->started) { s->started = true; json_free(args); store_call("projects", json_object(), 0, s, projects_done, 0, &s->read); return; }
    if (s->next_project < s->project_count && !s->capped) {
        json_set_str(args, "repo", s->projects[s->next_project].repo);
        store_call("pulls", args, 0, s, pulls_done, 0, &s->read); return;
    }
    s->next_project = s->project_count;
    if (s->next_pull < s->pull_count) {
        const SecurityPull *p = &s->pulls[s->next_pull];
        json_set_str(args, "repo", p->project.repo); json_set_num(args, "pr", p->number);
        store_call("code_review_rounds", args, 0, s, outcome_done, 0, &s->read); return;
    }
    json_free(args);
}
static void visible(Screen *base, bool shown) {
    SecurityScreen *s = (SecurityScreen *)base; s->shown = shown;
    if (!security_current(s)) { request_cancel(&s->read); return; }
    if (shown) step(s);
    else request_cancel(&s->read);   // the request's index is not advanced, so showing again reads it again
}
static void refresh(Screen *base) {
    SecurityScreen *s = (SecurityScreen *)base;
    clear(s); step(s); pane_relayout(base->pane);
}
static void header(Screen *base, HeaderInfo *info) {
    SecurityScreen *s = (SecurityScreen *)base;
    snprintf(info->title, sizeof info->title, "Security");
    snprintf(info->subtitle, sizeof info->subtitle, "Security review of open pull requests across your projects");
    if (security_current(s)) info->buttons[info->button_count++] = (HeaderButton){.glyph = 0xE72C, .action = SECURITY_REFRESH, .enabled = true, .tip = "Read every pull request again"};
}
static void action(Screen *base, int id, intptr_t arg, POINT pt) {
    (void)pt; SecurityScreen *s = (SecurityScreen *)base;
    if (!security_current(s)) return;
    if (id == SECURITY_REFRESH) { refresh(base); return; }
    size_t index = (size_t)(arg & 0xFFFF);
    if (id != SECURITY_OPEN || (unsigned)(arg >> 16) != (s->generation & 0xFFFF) || index >= s->pull_count) return;
    const SecurityPull *p = &s->pulls[index];
    app_push_from(base, pull_code_reviews_screen_new(&p->project, p->number));
}
static void activated(Screen *base, bool active) { if (active) visible(base, true); }
static void destroy(Screen *base) {
    SecurityScreen *s = (SecurityScreen *)base; clear(s);
    json_free(s->device); api_client_release(s->client); screen_release(base);
}
static const ScreenVTable VT = {.destroy = destroy, .layout = security_layout, .header = header, .action = action,
    .visible = visible, .refresh = refresh, .activated = activated, .detachable = true};
Screen *security_screen_new(void) {
    SecurityScreen *s = xcalloc(1, sizeof *s); s->base.vt = &VT;
    s->client = api_client_retain(g_store.client); s->device = device_json(&g_store.device);
    char *scope = json_serialize(s->device, true);
    s->base.id = xstrfmt("security:%p:%s", (void *)s->client, scope); free(scope);
    return &s->base;
}
