#include "project_github.h"
#include "github_account.h"
#include <stdlib.h>
#include <string.h>

bool project_github_offered(void) { return store_supports("github_account"); }

static bool health_offered(void) {
    return str_eq(g_store.device.permission, "admin") && store_supports("github_account_health");
}

void project_github_init(ProjectGithub *g, Screen *screen, const char *repo) {
    g->screen = screen; g->repo = xstrdup(repo);
}

void project_github_clear(ProjectGithub *g) {
    request_cancel(&g->identity_request); request_cancel(&g->health_request);
    api_client_release(g->client); free(g->repo); free(g->device); free(g->permission);
    json_free(g->account); json_free(g->health); free(g->identity_error); free(g->health_error);
    memset(g, 0, sizeof *g);
}

static bool current(ProjectGithub *g) {
    return store_connection_matches(g->client, g->device, g->permission) && project_github_offered();
}

static void prepare(ProjectGithub *g) {
    if (current(g)) return;
    Screen *screen = g->screen; char *repo = xstrdup(g->repo);
    project_github_clear(g); project_github_init(g, screen, repo); free(repo);
    g->client = api_client_retain(g_store.client); g->device = xstrdup(g_store.device.id);
    g->permission = xstrdup(g_store.device.permission);
}

static void identity_done(void *owner, Request *req) {
    ProjectGithub *g = owner;
    if (!current(g) || req->client != g->client) return;
    g->loaded = true;
    const Json *account = req->ok ? json_get(req->result, "account") : NULL;
    if (!json_is_object(account)) {
        char *text = request_error_or_unexpected(req); set_string(&g->identity_error, text); free(text);
    } else {
        json_free(g->account); g->account = json_clone(account); set_string(&g->identity_error, NULL);
    }
    pane_relayout(g->screen->pane);
}

void project_github_load(ProjectGithub *g, bool retry) {
    prepare(g);
    if (str_empty(g->repo) || !project_github_offered() || g->identity_request) return;
    if (g->loaded && !retry) return;
    g->loaded = false; set_string(&g->identity_error, NULL);
    Json *args = json_object(); json_set_str(args, "repo", g->repo);
    store_call("github_account", args, 0, g, identity_done, 0, &g->identity_request);
}

static void health_done(void *owner, Request *req) {
    ProjectGithub *g = owner;
    if (!current(g) || !health_offered() || req->client != g->client) return;
    if (!req->ok || json_bool_tristate(json_get(req->result, "healthy")) < 0) {
        char *text = request_error_or_unexpected(req); set_string(&g->health_error, text); free(text);
    } else {
        g->health = json_clone(req->result); set_string(&g->health_error, NULL);
    }
    pane_relayout(g->screen->pane);
}

void project_github_check(ProjectGithub *g) {
    prepare(g);
    if (str_empty(g->repo) || !project_github_offered() || !health_offered() || g->health_request) return;
    json_free(g->health); g->health = NULL; set_string(&g->health_error, NULL);
    Json *args = json_object(); json_set_str(args, "repo", g->repo);
    store_call("github_account_health", args, 0, g, health_done, 0, &g->health_request);
    pane_relayout(g->screen->pane);
}

static void summary(Doc *doc, int x, int w, char *text) {
    doc_text(doc, x, w, text, FONT_FOOTNOTE, theme.ink, DT_WORDBREAK); free(text);
    doc_space(doc, px(6));
}

void project_github_layout(ProjectGithub *g, Doc *doc, int x, int w) {
    project_github_load(g, false);
    if (str_empty(g->repo) || !project_github_offered()) return;
    doc_text(doc, x, w, "GitHub account", FONT_SUBHEADLINE, theme.ink, DT_LEFT);
    doc_space(doc, px(8));
    if (g->identity_request) doc_loading(doc, x, w, "Loading GitHub account…");
    else if (g->identity_error) {
        doc_notice_box(doc, x, w, g->identity_error);
        doc_button(doc, x, w, "Reload GitHub account", BUTTON_BORDERED, ACT_PROJECT_GITHUB_RELOAD, 0, true);
    } else if (g->account) summary(doc, x, w, github_account_identity(g->account));
    if (!health_offered()) { doc_space(doc, px(16)); return; }
    doc_button(doc, x, w, g->health_request ? "Checking GitHub access…" : "Check GitHub access",
        BUTTON_BORDERED, ACT_PROJECT_GITHUB_CHECK, 0, !g->health_request);
    doc_space(doc, px(8));
    if (g->health_error) doc_notice_box(doc, x, w, g->health_error);
    if (g->health) {
        const Json *h = g->health;
        summary(doc, x, w, github_account_health_overall(h));
        summary(doc, x, w, github_account_identity(json_get(h, "account")));
        summary(doc, x, w, github_account_health_token(h));
        char *scopes = github_account_health_scopes(h);
        summary(doc, x, w, xstrfmt("Scopes: %s", scopes)); free(scopes);
        const Json *repos = json_get(h, "repos");
        size_t count = json_count(repos);
        if (!count) summary(doc, x, w, xstrdup("No repositories checked"));
        for (size_t i = 0; i < count; i++) summary(doc, x, w, github_account_health_repo(json_at(repos, i)));
        time_t now = time(NULL);
        summary(doc, x, w, github_account_health_rate(h, "core", now));
        summary(doc, x, w, github_account_health_rate(h, "graphql", now));
    }
    doc_space(doc, px(16));
}
