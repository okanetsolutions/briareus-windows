#include "test.h"
#include "suites.h"
#include "github_account.h"
#include <stdlib.h>
#include <string.h>

static void test_github_account_metadata(void) {
    Json *account = json_parsez("{\"scopes\":null,\"repos\":[\"o/r\",\"o/second\"]}");
    CHECK_OWNED_STR(github_account_scopes(account), "scopes unknown (fine-grained token)");
    CHECK_OWNED_STR(github_account_projects(account), "o/r, o/second");
    json_object_set(account, "scopes", json_parsez("[\"repo\",null,\"\",\"read:org\"]"));
    CHECK_OWNED_STR(github_account_scopes(account), "repo, read:org");
    json_object_set(account, "scopes", json_array()); json_object_set(account, "repos", json_array());
    CHECK_OWNED_STR(github_account_scopes(account), "no classic scopes reported");
    CHECK_OWNED_STR(github_account_projects(account), "No assigned projects");
    json_free(account);
}

static void test_project_github_summaries(void) {
    Json *account = json_parsez("{\"globalFallback\":true,\"label\":\"Production\",\"login\":\"bot\"}");
    char *identity = github_account_identity(account);
    CHECK(strstr(identity, "GITHUB_TOKEN") && !strstr(identity, "@bot")); free(identity);
    json_set_bool(account, "globalFallback", false);
    identity = github_account_identity(account);
    CHECK(strstr(identity, "Production") && strstr(identity, "@bot")); free(identity);
    json_set_str(account, "label", ""); CHECK_OWNED_STR(github_account_identity(account), "@bot"); json_free(account);
    Json *h = json_parsez("{\"healthy\":true,\"token\":{\"status\":\"valid\",\"httpStatus\":200},"
        "\"scopes\":{\"status\":\"unknown\",\"granted\":null,\"missing\":null}}");
    char *overall = github_account_health_overall(h);
    CHECK(strstr(overall, "yes") != NULL); free(overall);
    char *token = github_account_health_token(h);
    CHECK(strstr(token, " valid") && strstr(token, "HTTP 200")); free(token);
    CHECK_OWNED_STR(github_account_health_scopes(h), "unknown");
    json_free(h);
    h = json_parsez("{\"healthy\":false,\"token\":{\"status\":\"rate_limited\",\"httpStatus\":null},"
        "\"scopes\":{\"status\":\"missing\",\"missing\":[\"repo\",\"admin:repo_hook\"]}}");
    overall = github_account_health_overall(h);
    CHECK(strstr(overall, "no") != NULL); free(overall);
    token = github_account_health_token(h);
    CHECK(strstr(token, "rate_limited") && !strstr(token, "HTTP")); free(token);
    char *scopes = github_account_health_scopes(h);
    CHECK(strstr(scopes, "missing") && strstr(scopes, "repo,") && strstr(scopes, "admin:repo_hook")); free(scopes);
    json_free(h); h = json_parsez("{\"scopes\":{\"status\":\"ok\"}}");
    CHECK_OWNED_STR(github_account_health_scopes(h), "ok");
    json_free(h);
    Json *repo = json_parsez("{\"repo\":\"o/denied\",\"status\":\"no_access\",\"accessible\":false,\"httpStatus\":403}");
    char *access = github_account_health_repo(repo);
    CHECK(strstr(access, "o/denied") && strstr(access, "no_access"));
    CHECK(strstr(access, "access denied") && strstr(access, "HTTP 403")); free(access);
    json_set_str(repo, "status", "rate_limited"); json_object_set(repo, "accessible", json_null()); json_object_set(repo, "httpStatus", json_null());
    access = github_account_health_repo(repo);
    CHECK(strstr(access, "rate_limited") && strstr(access, "access unknown") && !strstr(access, "HTTP"));
    free(access); json_free(repo);
}

static void test_project_github_rate_windows(void) {
    Json *h = json_parsez("{\"rateLimit\":{\"core\":{\"remaining\":4999,\"limit\":5000,\"resetAt\":1700000000000},"
        "\"graphql\":{\"remaining\":null,\"limit\":null,\"resetAt\":null},\"cooldownUntil\":{\"core\":1700000060000}}}");
    char *rate = github_account_health_rate(h, "core", 1700000000);
    CHECK(strstr(rate, "REST") && strstr(rate, "4999 / 5000"));
    CHECK(strstr(rate, "2023-11-14 22:13:20 UTC") && strstr(rate, "2023-11-14 22:14:20 UTC"));
    free(rate);
    rate = github_account_health_rate(h, "core", 1700000061);
    CHECK(strstr(rate, "2023-11-14 22:13:20 UTC") && !strstr(rate, "2023-11-14 22:14:20 UTC"));
    CHECK(!strstr(rate, "retry after")); free(rate);
    rate = github_account_health_rate(h, "graphql", 1700000061);
    CHECK(strstr(rate, "GraphQL") && strstr(rate, "unknown / unknown") && !strstr(rate, "retry after")); free(rate);
    json_free(h);
    rate = github_account_health_rate(NULL, "core", 1700000061);
    CHECK(strstr(rate, "REST") && strstr(rate, "unknown / unknown") && !strstr(rate, "retry after")); free(rate);
}

void core_github_account_tests(void) {
    test_run("Project GitHub identity, token, scopes and access diagnostics", test_project_github_summaries);
    test_run("Project GitHub rate diagnostics omit elapsed cooldowns", test_project_github_rate_windows);
    test_run("GitHub account nullable scopes and project summaries", test_github_account_metadata);
}
