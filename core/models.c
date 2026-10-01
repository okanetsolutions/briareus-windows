#include "models.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static char *dup_str(const Json *v) { const char *s = json_str(v); return s ? xstrdup(s) : NULL; }
static char **dup_string_array(const Json *v, size_t *count) {
    size_t n = json_count(v), m = 0;
    char **out = xmalloc((n ? n : 1) * sizeof *out);
    for (size_t i = 0; i < n; i++) { const char *s = json_str(json_at(v, i)); if (s) out[m++] = xstrdup(s); }
    *count = m;
    return out;
}
static Json *string_array_json(char **items, size_t count) {
    Json *a = json_array();
    for (size_t i = 0; i < count; i++) json_array_push(a, json_string(items[i]));
    return a;
}

// MARK: - Connection

bool device_parse(const Json *value, Device *out) {
    memset(out, 0, sizeof *out);
    const char *id = json_str(json_get(value, "id")), *label = json_str(json_get(value, "label")), *permission = json_str(json_get(value, "permission"));
    double expires;
    if (!id || !label || !permission || !json_is_array(json_get(value, "repos")) || !json_num(json_get(value, "expiresAt"), &expires)) return false;
    out->id = xstrdup(id); out->label = xstrdup(label); out->permission = xstrdup(permission);
    out->repos = dup_string_array(json_get(value, "repos"), &out->repo_count);
    out->expires_at = expires;
    return true;
}
Json *device_json(const Device *d) {
    Json *o = json_object();
    json_set_str(o, "id", d->id); json_set_str(o, "label", d->label); json_set_str(o, "permission", d->permission);
    json_object_set(o, "repos", string_array_json(d->repos, d->repo_count));
    json_set_num(o, "expiresAt", d->expires_at);
    return o;
}
void device_free(Device *d) {
    if (!d) return;
    free(d->id); free(d->label); free(d->permission); str_array_free(d->repos, d->repo_count);
    memset(d, 0, sizeof *d);
}
void device_copy(Device *into, const Device *from) {
    Json *j = device_json(from); device_parse(j, into); json_free(j);
}
int permission_rank(const char *permission) {
    if (str_eq(permission, "read")) return 0;
    if (str_eq(permission, "manage")) return 1;
    if (str_eq(permission, "admin")) return 2;
    return -1;
}
bool device_can_manage(const Device *d) { return d && permission_rank(d->permission) >= 1; }
time_t device_expiry(const Device *d) { return (time_t)(d->expires_at / 1000); }

bool discovery_parse(const Json *value, Discovery *out) {
    memset(out, 0, sizeof *out);
    out->version = json_int_or(json_get(value, "version"), -1);
    if (out->version < 0) return false;
    if (!device_parse(json_get(value, "client"), &out->device)) return false;
    out->transcribe = json_bool_tristate(json_get(value, "transcribe"));
    return true;
}
void discovery_free(Discovery *d) { if (d) device_free(&d->device); }
const char *discovery_voice_notes_off(int transcribe) {
    if (transcribe == 1) return NULL;
    if (transcribe == 0) return "Voice notes are off: the server needs OPENAI_TRANSCRIBE_API_KEY and OPENAI_TRANSCRIBE_MODEL, and a restart once they are set.";
    return "This server cannot transcribe voice notes yet. Update Briareus on the server to a version whose client API transcribes.";
}

static const char *const METHODS[] = { "GET", "POST", "PUT", "PATCH", "DELETE" };
static void routes_push(Route **routes, size_t *count, const char *method, const char *path, const char *access) {
    *routes = xrealloc(*routes, (*count + 1) * sizeof **routes);
    Route *r = &(*routes)[(*count)++];
    r->method = xstrdup(method); r->path = xstrdup(path); r->access = xstrdup(access);
}
bool routes_parse(const Json *value, Route **out, size_t *count) {
    Route *routes = xcalloc(1, sizeof *routes); size_t n = 0;
    if (json_is_array(value)) {
        // A saved list.
        for (size_t i = 0; i < json_count(value); i++) {
            const Json *entry = json_at(value, i);
            const char *method = json_str(json_get(entry, "method")), *path = json_str(json_get(entry, "path")), *access = json_str(json_get(entry, "access"));
            if (!method || !path || !access) { routes_free(routes, n); return false; }
            routes_push(&routes, &n, method, path, access);
        }
    } else {
        // The OpenAPI document: paths, each with its operations by lower-case method.
        const Json *paths = json_get(value, "paths");
        if (!json_is_object(paths)) { free(routes); return false; }
        for (size_t i = 0; i < json_count(paths); i++) {
            const char *path = json_key(paths, i);
            const Json *ops = json_get(paths, path);
            for (size_t m = 0; m < sizeof METHODS / sizeof *METHODS; m++) {
                char *lower = str_fold(METHODS[m]);
                const Json *op = json_get(ops, lower);
                free(lower);
                if (!json_is_object(op)) continue;
                const char *access = json_str(json_get(op, "x-briareus-access"));
                // A route that says nothing about who may call it is the operator's.
                routes_push(&routes, &n, METHODS[m], path, access ? access : "admin");
            }
        }
    }
    *out = routes; *count = n;
    return true;
}
Json *routes_json(const Route *routes, size_t count) {
    Json *a = json_array();
    for (size_t i = 0; i < count; i++) {
        Json *o = json_object();
        json_set_str(o, "method", routes[i].method); json_set_str(o, "path", routes[i].path); json_set_str(o, "access", routes[i].access);
        json_array_push(a, o);
    }
    return a;
}
void routes_free(Route *routes, size_t count) {
    if (!routes) return;
    for (size_t i = 0; i < count; i++) { free(routes[i].method); free(routes[i].path); free(routes[i].access); }
    free(routes);
}
Route *routes_copy(const Route *routes, size_t count) {
    Route *c = xcalloc(count ? count : 1, sizeof *c);
    for (size_t i = 0; i < count; i++) { c[i].method = xstrdup(routes[i].method); c[i].path = xstrdup(routes[i].path); c[i].access = xstrdup(routes[i].access); }
    return c;
}
/// Segment by segment, a parameter standing for any one segment. Leading and trailing slashes do not count.
static bool paths_match(const char *a, const char *b) {
    while (*a == '/') a++;
    while (*b == '/') b++;
    for (;;) {
        const char *ea = strchr(a, '/'), *eb = strchr(b, '/');
        size_t la = ea ? (size_t)(ea - a) : strlen(a), lb = eb ? (size_t)(eb - b) : strlen(b);
        bool param = (la && a[0] == '{') || (lb && b[0] == '{');
        if (!param && (la != lb || strncmp(a, b, la) != 0)) return false;
        if (param && (!la || !lb)) return false;
        a += la; b += lb;
        while (*a == '/') a++;
        while (*b == '/') b++;
        if (!*a || !*b) return !*a && !*b;
    }
}
bool routes_allow(const Route *routes, size_t count, const char *method, const char *path, const char *permission) {
    int rank = permission_rank(permission);
    if (rank < 0) return false;
    for (size_t i = 0; i < count; i++) {
        if (!str_ieq(routes[i].method, method) || !paths_match(routes[i].path, path)) continue;
        int needed = permission_rank(routes[i].access);
        return needed >= 0 && rank >= needed;
    }
    return false;
}

bool connection_parse(const Json *value, Connection *out) {
    memset(out, 0, sizeof *out);
    if (!device_parse(json_get(value, "device"), &out->device)) return false;
    if (!json_is_array(json_get(value, "routes")) || !routes_parse(json_get(value, "routes"), &out->routes, &out->route_count)) { device_free(&out->device); return false; }
    out->transcribe = json_bool_tristate(json_get(value, "transcribe"));
    return true;
}
Json *connection_json(const Connection *c) {
    Json *o = json_object();
    json_object_set(o, "device", device_json(&c->device));
    json_object_set(o, "routes", routes_json(c->routes, c->route_count));
    if (c->transcribe >= 0) json_set_bool(o, "transcribe", c->transcribe == 1);
    return o;
}
void connection_free(Connection *c) {
    if (!c) return;
    device_free(&c->device); routes_free(c->routes, c->route_count);
    memset(c, 0, sizeof *c);
}

// MARK: - Projects

bool project_parse(const Json *value, Project *out) {
    memset(out, 0, sizeof *out);
    const char *repo = json_str(json_get(value, "repo"));
    if (!repo) return false;
    out->repo = xstrdup(repo); out->label = dup_str(json_get(value, "label"));
    return true;
}
Json *project_json(const Project *p) {
    Json *o = json_object(); json_set_str(o, "repo", p->repo);
    json_object_set(o, "label", p->label ? json_string(p->label) : json_null());
    return o;
}
void project_free(Project *p) { if (!p) return; free(p->repo); free(p->label); memset(p, 0, sizeof *p); }
void project_copy(Project *into, const Project *from) { into->repo = xstrdup(from->repo); into->label = from->label ? xstrdup(from->label) : NULL; }
const char *project_title(const Project *p) { return str_empty(p->label) ? p->repo : p->label; }
bool projects_parse(const Json *value, Project **out, size_t *count) {
    const Json *list = json_is_array(value) ? value : json_get(value, "projects");
    if (!json_is_array(list)) return false;
    size_t n = json_count(list), m = 0;
    Project *items = xcalloc(n ? n : 1, sizeof *items);
    for (size_t i = 0; i < n; i++) if (project_parse(json_at(list, i), &items[m])) m++;
    *out = items; *count = m;
    return true;
}
Json *projects_json(const Project *projects, size_t count) {
    Json *a = json_array();
    for (size_t i = 0; i < count; i++) json_array_push(a, project_json(&projects[i]));
    return a;
}
void projects_free(Project *projects, size_t count) {
    if (!projects) return;
    for (size_t i = 0; i < count; i++) project_free(&projects[i]);
    free(projects);
}

// MARK: - Sessions

bool session_parse(const Json *value, Session *out) {
    out->raw = NULL;
    if (!json_str(json_get(value, "id")) || !json_str(json_get(value, "status"))) return false;
    out->raw = json_clone(value);
    return true;
}
void session_free(Session *s) { if (!s) return; json_free(s->raw); s->raw = NULL; }
void session_copy(Session *into, const Session *from) { into->raw = json_clone(from->raw); }
const char *session_id(const Session *s) { const char *v = json_str(json_get(s->raw, "id")); return v ? v : ""; }
const char *session_repo(const Session *s) { return json_str(json_get(s->raw, "repo")); }
const char *session_status(const Session *s) { const char *v = json_str(json_get(s->raw, "status")); return v ? v : ""; }
const char *session_model(const Session *s) { return json_str_nonempty(json_get(s->raw, "model")); }
const char *session_provider(const Session *s) { return json_str_nonempty(json_get(s->raw, "provider")); }
const char *session_display_title(const Session *s) {
    const char *t = json_str_nonempty(json_get(s->raw, "title"));
    return t ? t : "New conversation";
}
bool session_is_active(const Session *s) {
    const char *st = session_status(s);
    return str_eq(st, "queued") || str_eq(st, "preparing") || str_eq(st, "running") || str_eq(st, "starting");
}
bool session_live_input(const Session *s) { return json_bool_is(json_get(s->raw, "liveInput"), true); }
const Json *session_queued(const Session *s) { return json_get(s->raw, "queued"); }
bool session_review_loop_on(const Session *s) {
    return !json_is_null(json_get(s->raw, "reviewLoop"));
}
bool session_can_review_loop(const Session *s) {
    if (str_eq(session_status(s), "closed")) return false;
    static const char *const flags[] = { "reviewBranch", "qaBranch", "autoClose", "loopParentId", "local", "orchestrator" };
    for (size_t i = 0; i < sizeof flags / sizeof *flags; i++) if (json_is_set(json_get(s->raw, flags[i]))) return false;
    return true;
}
const Json *session_held_triage(const Session *s) {
    const Json *candidates[] = { json_get(s->raw, "reviewTriage"), json_get(json_get(s->raw, "reviewLoop"), "triage") };
    for (size_t i = 0; i < 2; i++) {
        if (!json_is_null(candidates[i]) && json_count(json_get(candidates[i], "findings")) > 0) return candidates[i];
    }
    return NULL;
}
int session_pull_number(const Session *s) {
    double n;
    if (!json_num(json_get(json_get(s->raw, "prStatus"), "number"), &n) && !json_num(json_get(s->raw, "startedOnPr"), &n)) return 0;
    return n >= 1 ? (int)n : 0;
}
bool sessions_parse(const Json *value, Session **out, size_t *count) {
    const Json *list = json_is_array(value) ? value : json_get(value, "sessions");
    if (!json_is_array(list)) return false;
    size_t n = json_count(list), m = 0;
    Session *items = xcalloc(n ? n : 1, sizeof *items);
    for (size_t i = 0; i < n; i++) if (session_parse(json_at(list, i), &items[m])) m++;
    *out = items; *count = m;
    return true;
}
Json *sessions_json(const Session *sessions, size_t count) {
    Json *a = json_array();
    for (size_t i = 0; i < count; i++) json_array_push(a, json_clone(sessions[i].raw));
    return a;
}
void sessions_free(Session *sessions, size_t count) {
    if (!sessions) return;
    for (size_t i = 0; i < count; i++) session_free(&sessions[i]);
    free(sessions);
}

// MARK: - Findings

const Json *session_held_round(const Session *s) {
    const Json *loop = json_get(json_get(s->raw, "reviewLoop"), "triage");
    if (json_is_object(loop)) return loop;
    const Json *standalone = json_get(s->raw, "reviewTriage");
    return json_is_object(standalone) ? standalone : NULL;
}
static const char *held_at(const Json *held) { const char *at = json_str(json_get(held, "heldAt")); return at ? at : ""; }
HeldRound *sessions_held_rounds(const Session *sessions, size_t count, size_t *out_count) {
    HeldRound *rounds = xcalloc(count ? count : 1, sizeof *rounds);
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        const Json *held = session_held_round(&sessions[i]);
        if (!held) continue;
        // Oldest hold first; equal holds keep the list's order.
        size_t at = n;
        while (at > 0 && strcmp(held_at(rounds[at - 1].held), held_at(held)) > 0) { rounds[at] = rounds[at - 1]; at--; }
        rounds[at].index = i; rounds[at].held = held;
        n++;
    }
    *out_count = n;
    return rounds;
}
int held_round_pr_number(const Json *held) { double n; return json_num(json_get(held, "prNumber"), &n) && n >= 1 ? (int)n : 0; }
char *held_round_pr_url(const Session *s, const Json *held) {
    const Json *pr = json_get(s->raw, "prStatus");
    const char *url = json_str_nonempty(json_get(pr, "url"));
    double number;
    int held_number = held_round_pr_number(held);
    if (url && json_num(json_get(pr, "number"), &number) && (int)number == held_number) return xstrdup(url);
    const char *repo = session_repo(s);
    return xstrfmt("https://github.com/%s/pull/%d", repo ? repo : "", held_number);
}
bool held_round_is_mine(const Json *held) {
    if (!json_is_set(json_get(held, "standalone"))) return true;
    return json_bool_is(json_get(held, "mine"), true);
}
char *triage_outcome_text(const Json *outcome, bool *danger) {
    *danger = false;
    if (json_is_set(json_get(outcome, "completed"))) {
        int pr = json_int_or(json_get(outcome, "prNumber"), 0);
        return pr ? xstrfmt("Review completed; what it found stays on PR #%d for its author.", pr)
                  : xstrdup("Review completed; what it found stays on the pull request for its author.");
    }
    if (json_is_set(json_get(outcome, "converged"))) return xstrdup("Verdicts recorded; nothing was left to fix, so code-approved was added and the loop converged.");
    if (json_is_set(json_get(outcome, "approved"))) return xstrdup("Verdicts recorded; nothing was left to fix, so code-approved was added.");
    if (json_is_set(json_get(outcome, "fixing"))) return xstrdup("Verdicts recorded; a fix session is running.");
    if (json_is_set(json_get(outcome, "reviewing"))) return xstrdup("Verdicts recorded; nothing was left to fix, but the branch had moved, so the new commits are being reviewed.");
    if (json_is_set(json_get(outcome, "deferred"))) return xstrdup("Verdicts recorded; nothing was left to fix, but the branch had moved. The new commits are reviewed once the session settles idle.");
    *danger = true;
    return xstrdup("Verdicts recorded, but no fix session started. The session\xE2\x80\x99s log says why.");
}
char *findings_subtitle(size_t rounds, size_t pull_requests) {
    if (!rounds) return xstrdup("nothing is waiting");
    if (pull_requests == rounds) return xstrfmt("%zu review%s waiting for a decision", rounds, rounds == 1 ? "" : "s");
    return xstrfmt("%zu reviews on %zu pull request%s waiting for a decision", rounds, pull_requests, pull_requests == 1 ? "" : "s");
}

// MARK: - Transcript

bool event_parse(const Json *value, Event *out) {
    memset(out, 0, sizeof *out);
    const char *kind = json_str(json_get(value, "kind"));
    double seq;
    if (!kind || !json_num(json_get(value, "seq"), &seq)) return false;
    out->raw = json_clone(value);
    out->seq = (int)seq;
    out->kind = json_str(json_get(out->raw, "kind"));
    out->t = json_str(json_get(out->raw, "t"));
    out->text = json_str(json_get(out->raw, "text"));
    out->name = json_str(json_get(out->raw, "name"));
    out->summary = json_str(json_get(out->raw, "summary"));
    out->question = json_str(json_get(out->raw, "question"));
    out->options = json_is_array(json_get(out->raw, "options")) ? json_get(out->raw, "options") : NULL;
    out->attachments = json_is_array(json_get(out->raw, "attachments")) ? json_get(out->raw, "attachments") : NULL;
    out->has_cost = json_num(json_get(out->raw, "costUsd"), &out->cost_usd);
    out->has_duration = json_num(json_get(out->raw, "durationMs"), &out->duration_ms);
    out->is_error = json_bool_tristate(json_get(out->raw, "isError"));
    return true;
}
void event_free(Event *e) { if (!e) return; json_free(e->raw); memset(e, 0, sizeof *e); }
const char *event_detail(const Event *e) { return e->text ? e->text : e->summary; }
bool event_visible(const Event *e) {
    if (str_eq(e->kind, "status")) return false;
    return e->text || e->question || str_eq(e->kind, "tool") || str_eq(e->kind, "tool_error") || str_eq(e->kind, "result");
}
bool event_time(const Event *e, time_t *out) { return board_date_parse(e->t, out); }

void transcript_init(Transcript *t) { memset(t, 0, sizeof *t); }
void transcript_free(Transcript *t) {
    if (!t) return;
    for (size_t i = 0; i < t->count; i++) event_free(&t->events[i]);
    free(t->events); transcript_init(t);
}
static int compare_events(const void *a, const void *b) {
    int x = ((const Event *)a)->seq, y = ((const Event *)b)->seq;
    return x < y ? -1 : x > y;
}
void transcript_append(Transcript *t, const Json *events) {
    size_t n = json_count(events);
    bool added = false;
    for (size_t i = 0; i < n; i++) {
        Event e;
        if (!event_parse(json_at(events, i), &e)) continue;
        bool seen = false;
        for (size_t j = 0; j < t->count; j++) if (t->events[j].seq == e.seq) { seen = true; break; }
        if (seen) { event_free(&e); continue; }
        if (t->count == t->cap) { t->cap = t->cap ? t->cap * 2 : 64; t->events = xrealloc(t->events, t->cap * sizeof *t->events); }
        t->events[t->count++] = e; added = true;
    }
    if (added) qsort(t->events, t->count, sizeof *t->events, compare_events);
    int last = t->count ? t->events[t->count - 1].seq : 0;
    if (last > t->cursor) t->cursor = last;
}
Json *transcript_json(const Transcript *t) {
    Json *a = json_array();
    for (size_t i = 0; i < t->count; i++) json_array_push(a, json_clone(t->events[i].raw));
    return a;
}

// MARK: - Runtimes

static void runtime_model_free(RuntimeModel *m) { free(m->id); free(m->label); str_array_free(m->efforts, m->effort_count); free(m->default_effort); }
static void runtime_provider_free(RuntimeProvider *p) {
    free(p->label); free(p->default_model);
    for (size_t i = 0; i < p->model_count; i++) runtime_model_free(&p->models[i]);
    free(p->models);
}
void runtime_choice_free(RuntimeChoice *c) { if (!c) return; free(c->model); free(c->effort); memset(c, 0, sizeof *c); }
void runtime_choice_copy(RuntimeChoice *into, const RuntimeChoice *from) {
    into->provider_id = from->provider_id; into->model = from->model ? xstrdup(from->model) : NULL; into->effort = from->effort ? xstrdup(from->effort) : NULL;
}
bool runtime_choice_equal(const RuntimeChoice *a, const RuntimeChoice *b) {
    return a->provider_id == b->provider_id && str_eq(a->model, b->model) && str_eq(a->effort, b->effort);
}
static bool runtime_choice_parse(const Json *value, RuntimeChoice *out) {
    memset(out, 0, sizeof *out);
    double id;
    if (!json_num(json_get(value, "providerId"), &id)) return false;
    out->provider_id = (int)id; out->model = dup_str(json_get(value, "model")); out->effort = dup_str(json_get(value, "effort"));
    return true;
}
static Json *runtime_choice_json(const RuntimeChoice *c) {
    Json *o = json_object(); json_set_num(o, "providerId", c->provider_id);
    json_object_set(o, "model", c->model ? json_string(c->model) : json_null());
    json_object_set(o, "effort", c->effort ? json_string(c->effort) : json_null());
    return o;
}
Json *runtime_choice_arguments(const RuntimeChoice *c) {
    Json *o = json_object(); json_set_num(o, "provider", c->provider_id);
    if (!str_empty(c->model)) json_set_str(o, "model", c->model);
    if (!str_empty(c->effort)) json_set_str(o, "effort", c->effort);
    return o;
}
bool runtime_catalog_parse(const Json *value, RuntimeCatalog *out) {
    memset(out, 0, sizeof *out);
    const Json *providers = json_get(value, "providers");
    if (!json_is_array(providers)) return false;
    out->has_default = runtime_choice_parse(json_get(value, "default"), &out->def);
    size_t n = json_count(providers);
    out->providers = xcalloc(n ? n : 1, sizeof *out->providers);
    for (size_t i = 0; i < n; i++) {
        const Json *pv = json_at(providers, i);
        double id;
        const char *label = json_str(json_get(pv, "label"));
        const Json *models = json_get(pv, "models");
        if (!json_num(json_get(pv, "id"), &id) || !label || !json_is_array(models)) { runtime_catalog_free(out); return false; }
        RuntimeProvider *p = &out->providers[out->provider_count++];
        p->id = (int)id; p->label = xstrdup(label); p->available = json_bool_tristate(json_get(pv, "available"));
        p->default_model = dup_str(json_get(pv, "defaultModel"));
        size_t mn = json_count(models);
        p->models = xcalloc(mn ? mn : 1, sizeof *p->models);
        for (size_t j = 0; j < mn; j++) {
            const Json *mv = json_at(models, j);
            const char *mid = json_str(json_get(mv, "id"));
            if (!mid) { runtime_catalog_free(out); return false; }
            RuntimeModel *m = &p->models[p->model_count++];
            m->id = xstrdup(mid); m->label = dup_str(json_get(mv, "label"));
            m->efforts = json_is_array(json_get(mv, "efforts")) ? dup_string_array(json_get(mv, "efforts"), &m->effort_count) : NULL;
            m->default_effort = dup_str(json_get(mv, "defaultEffort"));
        }
    }
    return true;
}
Json *runtime_catalog_json(const RuntimeCatalog *c) {
    Json *o = json_object();
    json_object_set(o, "default", c->has_default ? runtime_choice_json(&c->def) : json_null());
    Json *providers = json_array();
    for (size_t i = 0; i < c->provider_count; i++) {
        const RuntimeProvider *p = &c->providers[i];
        Json *pj = json_object();
        json_set_num(pj, "id", p->id); json_set_str(pj, "label", p->label);
        if (p->available >= 0) json_set_bool(pj, "available", p->available == 1);
        json_object_set(pj, "defaultModel", p->default_model ? json_string(p->default_model) : json_null());
        Json *models = json_array();
        for (size_t j = 0; j < p->model_count; j++) {
            const RuntimeModel *m = &p->models[j];
            Json *mj = json_object();
            json_set_str(mj, "id", m->id);
            json_object_set(mj, "label", m->label ? json_string(m->label) : json_null());
            json_object_set(mj, "efforts", m->efforts ? string_array_json(m->efforts, m->effort_count) : json_null());
            json_object_set(mj, "defaultEffort", m->default_effort ? json_string(m->default_effort) : json_null());
            json_array_push(models, mj);
        }
        json_object_set(pj, "models", models);
        json_array_push(providers, pj);
    }
    json_object_set(o, "providers", providers);
    return o;
}
void runtime_catalog_free(RuntimeCatalog *c) {
    if (!c) return;
    runtime_choice_free(&c->def);
    for (size_t i = 0; i < c->provider_count; i++) runtime_provider_free(&c->providers[i]);
    free(c->providers);
    memset(c, 0, sizeof *c);
}
const char *runtime_model_title(const RuntimeModel *m) { return str_empty(m->label) ? m->id : m->label; }
bool runtime_provider_available(const RuntimeProvider *p) { return p->available != 0; }
const RuntimeProvider *runtime_catalog_provider(const RuntimeCatalog *c, int id) {
    for (size_t i = 0; i < c->provider_count; i++) if (c->providers[i].id == id) return &c->providers[i];
    return NULL;
}
const RuntimeModel *runtime_catalog_model(const RuntimeCatalog *c, const RuntimeChoice *choice) {
    const RuntimeProvider *p = runtime_catalog_provider(c, choice->provider_id);
    if (!p || !choice->model) return NULL;
    for (size_t i = 0; i < p->model_count; i++) if (str_eq(p->models[i].id, choice->model)) return &p->models[i];
    return NULL;
}
const char *const *runtime_catalog_efforts(const RuntimeCatalog *c, const RuntimeChoice *choice, size_t *count) {
    const RuntimeModel *m = runtime_catalog_model(c, choice);
    if (!m || !m->efforts) { *count = 0; return NULL; }
    *count = m->effort_count;
    return (const char *const *)m->efforts;
}
bool runtime_catalog_choice(const RuntimeCatalog *c, int provider_id, const char *model, RuntimeChoice *out) {
    const RuntimeProvider *p = runtime_catalog_provider(c, provider_id);
    if (!p) return false;
    const RuntimeModel *picked = NULL;
    for (size_t i = 0; i < p->model_count && !picked; i++) if (model && str_eq(p->models[i].id, model)) picked = &p->models[i];
    for (size_t i = 0; i < p->model_count && !picked; i++) if (p->default_model && str_eq(p->models[i].id, p->default_model)) picked = &p->models[i];
    if (!picked && p->model_count) picked = &p->models[0];
    memset(out, 0, sizeof *out);
    out->provider_id = provider_id;
    if (picked) {
        out->model = xstrdup(picked->id);
        const char *effort = picked->default_effort ? picked->default_effort : (picked->effort_count ? picked->efforts[0] : NULL);
        out->effort = effort ? xstrdup(effort) : NULL;
    }
    return true;
}
bool runtime_catalog_first_available(const RuntimeCatalog *c, RuntimeChoice *out) {
    for (size_t i = 0; i < c->provider_count; i++)
        if (runtime_provider_available(&c->providers[i])) return runtime_catalog_choice(c, c->providers[i].id, NULL, out);
    return false;
}
char *runtime_catalog_label(const RuntimeCatalog *c, const RuntimeChoice *choice) {
    const RuntimeProvider *p = runtime_catalog_provider(c, choice->provider_id);
    const RuntimeModel *m = runtime_catalog_model(c, choice);
    const char *model = m ? runtime_model_title(m) : choice->model;
    Str s; str_init(&s);
    if (p && !str_empty(p->label)) str_appendz(&s, p->label);
    if (!str_empty(model)) { if (s.len) str_appendz(&s, " \xC2\xB7 "); str_appendz(&s, model); }
    return str_detach(&s);
}

// MARK: - Pull request files

bool pull_file_parse(const Json *value, PullFile *out) {
    memset(out, 0, sizeof *out);
    const char *filename = json_str(json_get(value, "filename"));
    if (!filename) return false;
    out->filename = xstrdup(filename);
    out->previous_filename = dup_str(json_get(value, "previousFilename"));
    out->status = dup_str(json_get(value, "status"));
    out->patch = dup_str(json_get(value, "patch"));
    out->url = dup_str(json_get(value, "url"));
    out->additions = json_int_or(json_get(value, "additions"), -1);
    out->deletions = json_int_or(json_get(value, "deletions"), -1);
    return true;
}
Json *pull_file_json(const PullFile *f) {
    Json *o = json_object();
    json_set_str(o, "filename", f->filename);
    json_object_set(o, "previousFilename", f->previous_filename ? json_string(f->previous_filename) : json_null());
    json_object_set(o, "status", f->status ? json_string(f->status) : json_null());
    json_object_set(o, "additions", f->additions >= 0 ? json_number(f->additions) : json_null());
    json_object_set(o, "deletions", f->deletions >= 0 ? json_number(f->deletions) : json_null());
    json_object_set(o, "patch", f->patch ? json_string(f->patch) : json_null());
    json_object_set(o, "url", f->url ? json_string(f->url) : json_null());
    return o;
}
void pull_file_free(PullFile *f) {
    if (!f) return;
    free(f->filename); free(f->previous_filename); free(f->status); free(f->patch); free(f->url);
    memset(f, 0, sizeof *f);
}
void pull_file_copy(PullFile *into, const PullFile *from) { Json *j = pull_file_json(from); pull_file_parse(j, into); json_free(j); }
const char *pull_file_name(const PullFile *f) {
    const char *slash = strrchr(f->filename, '/');
    return slash && slash[1] ? slash + 1 : (slash ? f->filename : f->filename);
}
char *pull_file_directory(const PullFile *f) {
    const char *slash = strrchr(f->filename, '/');
    return slash ? xstrndup(f->filename, (size_t)(slash - f->filename)) : xstrdup("");
}

static bool parse_files(const Json *list, PullFile **out, size_t *count) {
    if (!json_is_array(list)) return false;
    size_t n = json_count(list), m = 0;
    PullFile *files = xcalloc(n ? n : 1, sizeof *files);
    for (size_t i = 0; i < n; i++) if (pull_file_parse(json_at(list, i), &files[m])) m++;
    *out = files; *count = m;
    return true;
}
static void free_files(PullFile *files, size_t count) { for (size_t i = 0; i < count; i++) pull_file_free(&files[i]); free(files); }

bool pull_files_page_parse(const Json *value, PullFilesPage *out) {
    memset(out, 0, sizeof *out);
    if (!json_is_object(value) || !parse_files(json_get(value, "files"), &out->files, &out->file_count)) return false;
    out->pr = json_clone(json_get(value, "pr"));
    out->next_page = json_int_or(json_get(value, "nextPage"), 0);
    out->truncated = json_bool_is(json_get(value, "truncated"), true);
    return true;
}
void pull_files_page_free(PullFilesPage *p) { if (!p) return; json_free(p->pr); free_files(p->files, p->file_count); memset(p, 0, sizeof *p); }

void pull_file_list_init(PullFileList *l) { memset(l, 0, sizeof *l); l->pr = json_null(); l->next_page = 1; }
void pull_file_list_free(PullFileList *l) { if (!l) return; json_free(l->pr); free_files(l->files, l->file_count); memset(l, 0, sizeof *l); }
bool pull_file_list_parse(const Json *value, PullFileList *out) {
    pull_file_list_init(out);
    if (!json_is_object(value) || !parse_files(json_get(value, "files"), &out->files, &out->file_count)) { pull_file_list_free(out); return false; }
    json_free(out->pr); out->pr = json_clone(json_get(value, "pr"));
    out->next_page = json_int_or(json_get(value, "nextPage"), 0);
    out->truncated = json_bool_is(json_get(value, "truncated"), true);
    return true;
}
Json *pull_file_list_json(const PullFileList *l) {
    Json *o = json_object();
    json_object_set(o, "pr", json_clone(l->pr));
    Json *files = json_array();
    for (size_t i = 0; i < l->file_count; i++) json_array_push(files, pull_file_json(&l->files[i]));
    json_object_set(o, "files", files);
    json_object_set(o, "nextPage", l->next_page ? json_number(l->next_page) : json_null());
    json_set_bool(o, "truncated", l->truncated);
    return o;
}
Json *pull_file_list_arguments(const PullFileList *l, const char *repo, int number) {
    if (!l->next_page) return NULL;
    Json *args = json_object();
    json_set_str(args, "repo", repo); json_set_num(args, "pr", number);
    if (l->next_page > 1) {
        json_set_num(args, "page", l->next_page);
        json_set_str(args, "headSha", json_str(json_get(l->pr, "headSha")));
        json_set_str(args, "baseSha", json_str(json_get(l->pr, "baseSha")));
    }
    return args;
}
void pull_file_list_append(PullFileList *l, const PullFilesPage *page) {
    if (l->file_count == 0) { json_free(l->pr); l->pr = json_clone(page->pr); }
    for (size_t i = 0; i < page->file_count; i++) {
        bool seen = false;
        for (size_t j = 0; j < l->file_count; j++) if (str_eq(l->files[j].filename, page->files[i].filename)) { seen = true; break; }
        if (seen) continue;
        l->files = xrealloc(l->files, (l->file_count + 1) * sizeof *l->files);
        pull_file_copy(&l->files[l->file_count++], &page->files[i]);
    }
    l->next_page = page->next_page; l->truncated = page->truncated;
}
bool pull_file_list_confirm(PullFileList *l, const PullFilesPage *page) {
    const char *head = json_str(json_get(page->pr, "headSha"));
    if (!head || !str_eq(head, json_str(json_get(l->pr, "headSha")))) return false;
    if (!json_equal(json_get(page->pr, "baseSha"), json_get(l->pr, "baseSha"))) return false;
    json_free(l->pr); l->pr = json_clone(page->pr);
    return true;
}

// MARK: - Dates

static bool read_digits(const char **p, int count, int *out) {
    int v = 0;
    for (int i = 0; i < count; i++) { if ((*p)[i] < '0' || (*p)[i] > '9') return false; v = v * 10 + ((*p)[i] - '0'); }
    *p += count; *out = v;
    return true;
}
static long long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
bool board_date_parse(const char *value, time_t *out) {
    if (!value) return false;
    const char *p = value;
    int y, mo, d, h, mi, s;
    if (!read_digits(&p, 4, &y) || *p++ != '-' || !read_digits(&p, 2, &mo) || *p++ != '-' || !read_digits(&p, 2, &d)) return false;
    if (*p != 'T' && *p != 't' && *p != ' ') return false;
    p++;
    if (!read_digits(&p, 2, &h) || *p++ != ':' || !read_digits(&p, 2, &mi)) return false;
    s = 0;
    if (*p == ':') { p++; if (!read_digits(&p, 2, &s)) return false; }
    if (*p == '.' || *p == ',') { p++; if (*p < '0' || *p > '9') return false; while (*p >= '0' && *p <= '9') p++; }
    long long offset = 0;
    if (*p == 'Z' || *p == 'z') p++;
    else if (*p == '+' || *p == '-') {
        int sign = *p == '-' ? -1 : 1; p++;
        int oh, om = 0;
        if (!read_digits(&p, 2, &oh)) return false;
        if (*p == ':') p++;
        if (*p >= '0' && *p <= '9' && !read_digits(&p, 2, &om)) return false;
        offset = sign * (oh * 3600LL + om * 60LL);
    } else return false;
    if (*p) return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) return false;
    long long days = days_from_civil(y, mo, d);
    *out = (time_t)(days * 86400 + h * 3600 + mi * 60 + s - offset);
    return true;
}
