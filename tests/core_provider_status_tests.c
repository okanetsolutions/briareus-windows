#include "provider_status.h"
#include "suites.h"
#include "test.h"

static void login_landed(void) {
    Json *status = json_parsez("{\"auth\":{\"loggedIn\":true,\"email\":\"one@example.com\",\"checkedAt\":\"later\"}}");
    Json *from = json_parsez("{\"loggedIn\":true,\"email\":\"one@example.com\",\"checkedAt\":\"earlier\"}");
    CHECK(!provider_login_landed(from, status));
    json_free(status); status = json_parsez("{\"auth\":{\"loggedIn\":true,\"email\":\"two@example.com\"}}");
    CHECK(provider_login_landed(from, status));
    json_free(from); from = json_parsez("{\"loggedIn\":false}");
    CHECK(provider_login_landed(from, status));
    CHECK(provider_login_landed(NULL, status));
    json_free(status); status = json_parsez("{\"auth\":{\"loggedIn\":false}}");
    CHECK(!provider_login_landed(from, status));
    CHECK(!provider_login_landed(from, NULL));
    json_free(status); status = json_parsez("{\"auth\":{\"loggedIn\":\"true\"}}");
    CHECK(!provider_login_landed(from, status));
    json_free(from); json_free(status);
}
void provider_status_tests(void) { test_run("provider login requires a new auth state", login_landed); }
