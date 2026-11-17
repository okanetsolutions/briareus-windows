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

void core_github_account_tests(void) {
    test_run("GitHub account nullable scopes and project summaries", test_github_account_metadata);
}
