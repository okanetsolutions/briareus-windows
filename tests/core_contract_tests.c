// Final October core catalog and synthetic responses: client compatibility, never provider execution.
#include "api.h"
#include "board.h"
#include "json.h"
#include "markdown.h"
#include "models.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include "fixtures/october-2026/catalog.h"
#include "fixtures/october-2026/compatibility.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static Json *catalog(void) {
    Str s; str_init(&s);
    for (size_t i = 0; i < sizeof OCTOBER_CATALOG / sizeof *OCTOBER_CATALOG; i++) str_appendz(&s, OCTOBER_CATALOG[i]);
    Json *j = json_parsez(s.data); str_free(&s); CHECK(j != NULL); return j;
}

static void test_final_catalog_permissions_and_absent_routes(void) {
    Json *j = catalog(); Route *routes = NULL; size_t n = 0;
    CHECK(routes_parse(j, &routes, &n));
    CHECK_STR(json_str(json_get(j, "x-core-commit")), "8079fcd341e87dba7ede9b8a9a6a534c5f8338cb");
    const char *permissions[] = { "read", "manage", "admin" };
    for (size_t i = 0; i < n; i++) {
        bool admin = str_has_prefix(routes[i].path, "/settings/") || str_has_prefix(routes[i].path, "/mail/")
            || str_has_prefix(routes[i].path, "/slack/") || str_eq(routes[i].path, "/usage/all");
        CHECK_STR(routes[i].access, admin ? "admin" : str_eq(routes[i].method, "GET") ? "read" : "manage");
        for (size_t p = 0; p < 3; p++) {
            bool allowed = admin ? p == 2 : str_eq(routes[i].method, "GET") || p >= 1;
            CHECK(routes_allow(routes, n, routes[i].method, routes[i].path, permissions[p]) == allowed);
            CHECK(!routes_allow(NULL, 0, routes[i].method, routes[i].path, permissions[p]));
        }
        CHECK(!routes_allow(routes, n, routes[i].method, routes[i].path, "owner"));
        // Removing just this operation must gate it even when adjacent operations remain.
        Route *without = xmalloc(n * sizeof *without); size_t count = 0;
        for (size_t k = 0; k < n; k++) if (k != i) without[count++] = routes[k];
        CHECK(!routes_allow(without, count, routes[i].method, routes[i].path, "admin"));
        free(without);
    }
    CHECK(routes_allow(routes, n, "GET", "mail/accounts/4/messages/msg%2F1", "admin"));
    CHECK(!routes_allow(routes, n, "POST", "mail/messages", "admin"));
    CHECK(!routes_allow(routes, n, "POST", "mail/accounts/4/messages/msg/read", "admin"));
    CHECK(routes_allow(routes, n, "GET", "slack/workspaces/1/conversations/C1/threads/123.456", "admin"));
    CHECK(!routes_allow(routes, n, "GET", "slack/workspaces/1/events", "manage"));
    CHECK(routes_allow(routes, n, "GET", "repo/archive", "read"));
    CHECK(api_route("repo_archive") == NULL); // Archive transport remains deliberately disabled.
    Json *saved = routes_json(routes, n); Route *back = NULL; size_t count = 0;
    CHECK(routes_parse(saved, &back, &count)); CHECK_INT(count, n);
    for (size_t i = 0; i < count; i++) { CHECK_STR(back[i].path, routes[i].path); CHECK_STR(back[i].access, routes[i].access); }
    routes_free(back, count); json_free(saved); routes_free(routes, n); json_free(j);
}

static const Json *response_schema(const Json *j, const char *path, const char *method, const char *status) {
    const Json *operation = json_get(json_get(json_get(j, "paths"), path), method);
    return json_get(json_get(json_get(json_get(json_get(operation, "responses"), status), "content"), "application/json"), "schema");
}
static size_t check_schema_references(const Json *value, const Json *schemas) {
    size_t refs = 0;
    const char *ref = json_str(json_get(value, "$ref"));
    if (ref) {
        const char *prefix = "#/components/schemas/";
        CHECK(str_has_prefix(ref, prefix));
        if (str_has_prefix(ref, prefix)) CHECK(!json_is_null(json_get(schemas, ref + strlen(prefix))));
        refs++;
    }
    for (size_t i = 0; i < json_count(value); i++) {
        const Json *child = json_is_array(value) ? json_at(value, i) : json_get(value, json_key(value, i));
        refs += check_schema_references(child, schemas);
    }
    return refs;
}
static void test_final_catalog_shapes(void) {
    Json *j = catalog();
    const Json *schemas = json_get(json_get(j, "components"), "schemas");
    CHECK(check_schema_references(j, schemas) > 0);
    CHECK(!json_is_null(json_get(schemas, "TranscriptEvent")));
    CHECK(!json_is_null(json_get(schemas, "MailMessage")));
    const Json *p = json_get(response_schema(j, "/settings/mail/accounts", "get", "200"), "properties");
    CHECK(!json_is_null(json_get(p, "accounts"))); CHECK(!json_is_null(json_get(p, "providers")));
    CHECK(!json_is_null(json_get(p, "callbackUrl"))); CHECK(!json_is_null(json_get(p, "defaults")));
    p = json_get(response_schema(j, "/settings/mail/accounts/connect", "post", "200"), "properties");
    CHECK(!json_is_null(json_get(p, "finishesOnServer"))); CHECK(!json_is_null(json_get(p, "expiresAt")));
    CHECK(!json_is_null(json_get(p, "redirectUri"))); CHECK(!json_is_null(json_get(p, "state")));
    p = json_get(response_schema(j, "/settings/mail/accounts/connect/finish", "post", "201"), "properties");
    CHECK_STR(json_str(json_get(json_get(p, "account"), "$ref")), "#/components/schemas/MailAccount");
    p = json_get(response_schema(j, "/settings/mail/accounts/{id}/sync", "post", "202"), "properties");
    CHECK(!json_is_null(json_get(p, "account")));
    p = json_get(response_schema(j, "/settings/mcp/servers/{id}/connect", "post", "200"), "properties");
    CHECK_STR(json_str(json_get(json_get(p, "server"), "$ref")), "#/components/schemas/McpServer");
    p = json_get(json_get(json_get(json_get(j, "components"), "schemas"), "McpServer"), "properties");
    CHECK(!json_is_null(json_get(p, "signInNeedsPaste"))); CHECK(!json_is_null(json_get(p, "headerNames")));
    p = json_get(response_schema(j, "/slack/workspaces/{id}/conversations/{channel}/messages", "get", "200"), "properties");
    CHECK(!json_is_null(json_get(p, "messages"))); CHECK(!json_is_null(json_get(p, "nextCursor"))); CHECK(!json_is_null(json_get(p, "hasMore")));
    const Json *events = json_get(json_get(json_get(json_get(json_get(json_get(j, "paths"), "/slack/workspaces/{id}/events"), "get"), "responses"), "200"), "content");
    CHECK(!json_is_null(json_get(events, "text/event-stream")));
    json_free(j);
}

static void test_claude_turn_costs_and_future_events_round_trip(void) {
    const double costs[] = { 0.04, 0.25, 0.25, 0.02, 0 };
    Json *j = json_parsez(CLAUDE_EVENTS); Transcript t; transcript_init(&t);
    transcript_append(&t, j); transcript_append(&t, j);
    CHECK_INT(t.count, 7); CHECK_INT(t.cursor, 7);
    for (size_t i = 0; i < 5; i++) {
        CHECK(t.events[i].has_cost); CHECK(fabs(t.events[i].cost_usd - costs[i]) < 1e-12);
        CHECK(event_visible(&t.events[i]));
    }
    CHECK_STR(t.events[3].kind, "btw_answer"); CHECK_STR(event_detail(&t.events[3]), "ok");
    CHECK(!t.events[5].has_cost); CHECK_INT(t.events[5].is_error, 1);
    CHECK(event_visible(&t.events[6]));
    Json *saved = transcript_json(&t); CHECK(json_equal(saved, j));
    Transcript back; transcript_init(&back); transcript_append(&back, saved);
    CHECK_INT(back.cursor, 7); CHECK(!back.events[5].has_cost);
    json_free(saved); transcript_free(&back); transcript_free(&t); json_free(j);
}

static void test_codex_server_rollup_preserves_absorbed_estimates_and_refresh(void) {
    Json *j = json_parsez(CODEX_SESSIONS); Session *sessions = NULL; size_t n = 0;
    CHECK(sessions_parse(j, &sessions, &n)); CHECK_INT(n, 3);
    if (n == 3) {
        const Json *u = json_get(sessions[0].raw, "usage");
        CHECK(json_is_null(json_get(sessions[0].raw, "costUsd")));
        CHECK(fabs(json_num_or(json_get(u, "costUsd"), 0) - 1.25) < 1e-12);
        CHECK(fabs(json_num_or(json_get(u, "estimatedCostUsd"), 0) - 0.25) < 1e-12);
        CHECK_INT(json_int_or(json_get(u, "sessions"), 0), 1); CHECK_INT(json_int_or(json_get(u, "unpricedTurns"), 0), 2);
        CHECK(json_is_null(json_get(json_get(sessions[1].raw, "usage"), "costUsd")));
        CHECK(json_is_null(json_get(sessions[2].raw, "usage")));
        Json *saved = sessions_json(sessions, n); CHECK(json_equal(saved, j)); json_free(saved);
        // A refreshed total replaces the cached server answer, without adding its estimate again.
        Json *next = json_clone(json_at(j, 0)); Json *next_usage = json_clone(json_get(next, "usage"));
        json_set_num(next_usage, "costUsd", 1.5); json_object_set(next, "usage", next_usage);
        Session refreshed; CHECK(session_parse(next, &refreshed)); session_free(&sessions[0]); session_copy(&sessions[0], &refreshed);
        session_free(&refreshed); json_free(next);
        CHECK(json_num_or(json_get(json_get(sessions[0].raw, "usage"), "costUsd"), 0) == 1.5);
    }
    sessions_free(sessions, n); json_free(j);
    j = json_parsez(CODEX_EVENT); Event e; CHECK(event_parse(j, &e));
    CHECK(e.has_cost); CHECK(e.cost_usd == 1.25); CHECK(event_visible(&e));
    CHECK(json_bool_is(json_get(e.raw, "costEstimated"), true));
    CHECK(json_equal(e.raw, j)); event_free(&e); json_free(j);
}

static void test_provider_choices_survive_catalog_round_trip(void) {
    Json *j = json_parsez(RUNTIMES); RuntimeCatalog c;
    CHECK(runtime_catalog_parse(j, &c)); CHECK_INT(c.provider_count, 2);
    RuntimeChoice choice = {0}; CHECK(runtime_catalog_choice(&c, 2, "gpt-5.6-sol", &choice));
    CHECK_INT(choice.provider_id, 2); CHECK_STR(choice.model, "gpt-5.6-sol"); CHECK_STR(choice.effort, "high");
    Json *saved = runtime_catalog_json(&c); RuntimeCatalog back; CHECK(runtime_catalog_parse(saved, &back));
    RuntimeChoice offered = {0}; CHECK(runtime_catalog_offered(&back, &choice, &offered));
    CHECK_INT(offered.provider_id, 2); runtime_choice_free(&offered);
    // A provider removed/disabled by a refreshed server is no longer startable.
    back.providers[1].available = 0; CHECK(!runtime_catalog_offered(&back, &choice, &offered));
    runtime_choice_free(&choice); runtime_catalog_free(&back); json_free(saved); runtime_catalog_free(&c); json_free(j);
}

static void test_not_fixed_prose_and_links_do_not_add_held_findings(void) {
    Json *j = json_parsez(REVIEW); const char *body = json_str(json_get(j, "body"));
    size_t n = 0; MdBlock *blocks = md_parse(body, &n); bool heading = false; size_t links = 0;
    for (size_t i = 0; i < n; i++) {
        if (blocks[i].kind == MD_HEADING && blocks[i].level == 3 && str_eq(blocks[i].text, "Not fixed")) heading = true;
        if (!blocks[i].text) continue;
        size_t sn = 0; MdSpan *spans = md_inline(blocks[i].text, &sn);
        for (size_t k = 0; k < sn; k++) if (spans[k].flags & SPAN_LINK) {
            CHECK(safe_web_url(spans[k].url));
            CHECK_STR(spans[k].url, links ? "https://github.com/o/r/pull/7#issuecomment-2" : "https://github.com/o/r/pull/7#discussion_r1"); links++;
        }
        md_spans_free(spans, sn);
    }
    CHECK(heading); CHECK_INT(links, 2); md_free(blocks, n);
    Session s; CHECK(session_parse(json_get(j, "session"), &s));
    const Json *findings = json_get(session_held_triage(&s), "findings");
    CHECK_INT(json_count(findings), 1); CHECK_STR(json_str(json_get(json_at(findings, 0), "key")), "optional");
    CHECK(json_bool_is(json_get(json_get(json_at(findings, 0), "assessment"), "worthFixing"), false));
    CHECK_INT(json_count(json_get(j, "findings")), 1);
    CHECK(json_equal(s.raw, json_get(j, "session")));
    session_free(&s); json_free(j);
}

void contract_tests(void) {
    test_run("final core catalog permissions and absent routes", test_final_catalog_permissions_and_absent_routes);
    test_run("final core catalog response shapes", test_final_catalog_shapes);
    test_run("Claude server turn costs and future events round trip", test_claude_turn_costs_and_future_events_round_trip);
    test_run("Codex absorbed estimates survive cache and refresh", test_codex_server_rollup_preserves_absorbed_estimates_and_refresh);
    test_run("provider choices survive refreshed catalogs", test_provider_choices_survive_catalog_round_trip);
    test_run("Not fixed prose and links keep only confirmed held findings", test_not_fixed_prose_and_links_do_not_add_held_findings);
}
