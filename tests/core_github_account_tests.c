#include "test.h"
#include "suites.h"
#include "github_account.h"
#include <stdlib.h>

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
    Json *account = json_parsez("{\"globalFallback\":true,\"login\":null}");
    CHECK_OWNED_STR(github_account_identity(account), "Server default (GITHUB_TOKEN)");
    json_set_bool(account, "globalFallback", false); json_set_str(account, "label", "Production"); json_set_str(account, "login", "bot");
    CHECK_OWNED_STR(github_account_identity(account), "Production (@bot)");
    json_set_str(account, "label", ""); CHECK_OWNED_STR(github_account_identity(account), "@bot"); json_free(account);
    Json *h = json_parsez("{\"scopes\":{\"status\":\"unknown\",\"granted\":null,\"missing\":null}}");
    CHECK_OWNED_STR(github_account_health_scopes(h), "unknown");
    json_free(h); h = json_parsez("{\"scopes\":{\"status\":\"missing\",\"missing\":[\"repo\",\"admin:repo_hook\"]}}");
    CHECK_OWNED_STR(github_account_health_scopes(h), "missing: repo, admin:repo_hook");
    json_free(h); h = json_parsez("{\"scopes\":{\"status\":\"ok\"},\"rateLimit\":{\"core\":{\"remaining\":4999,\"limit\":5000,\"resetAt\":1700000000000},\"graphql\":{\"remaining\":null,\"limit\":null,\"resetAt\":null},\"cooldownUntil\":{\"core\":1700000060000}}}");
    CHECK_OWNED_STR(github_account_health_scopes(h), "ok");
    CHECK_OWNED_STR(github_account_health_rate(h, "core"), "REST: 4999 / 5000 remaining; resets 2023-11-14 22:13:20 UTC; retry after 2023-11-14 22:14:20 UTC");
    CHECK_OWNED_STR(github_account_health_rate(h, "graphql"), "GraphQL: unknown / unknown remaining; resets unknown");
    json_free(h); CHECK_OWNED_STR(github_account_health_rate(NULL, "core"), "REST: unknown / unknown remaining; resets unknown");
    Json *repo = json_parsez("{\"repo\":\"o/denied\",\"status\":\"no_access\",\"accessible\":false,\"httpStatus\":403}");
    CHECK_OWNED_STR(github_account_health_repo(repo), "o/denied: no_access (access denied; HTTP 403)");
    json_set_str(repo, "status", "rate_limited"); json_object_set(repo, "accessible", json_null()); json_object_set(repo, "httpStatus", json_null());
    CHECK_OWNED_STR(github_account_health_repo(repo), "o/denied: rate_limited (access unknown)"); json_free(repo);
}

void core_github_account_tests(void) {
    test_run("Project GitHub identity scopes access and rate diagnostics", test_project_github_summaries);
    test_run("GitHub account nullable scopes and project summaries", test_github_account_metadata);
}
