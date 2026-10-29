#include "test.h"
#include "suites.h"
#include "ssh_servers.h"
#include "str.h"
#include <stdlib.h>

static void folders_and_filter(void) {
    Project *projects = NULL; size_t n = 0;
    Json *p = json_parsez("[{\"repo\":\"z/repo\",\"label\":\"First project\"},{\"repo\":\"a/repo\",\"label\":\"Second\"}]");
    CHECK(projects_parse(p, &projects, &n)); json_free(p);
    Json *rows = json_parsez("[{\"repo\":\"a/repo\",\"host\":\"prod.example\"},{\"label\":\"Unassigned\"},{\"repo\":\"z/repo\",\"username\":\"DEPLOY\"},{\"repo\":\"other/z\"},{\"repo\":\"other/a\"},{\"repo\":\"z/repo\",\"label\":\"Staging\"}]");
    Json *f = ssh_server_folders(rows, projects, n, "  ");
    CHECK_INT(json_count(f), 5);
    CHECK_STR(json_str(json_get(json_at(f, 0), "repo")), "z/repo");
    CHECK_STR(json_str(json_get(json_at(f, 1), "repo")), "a/repo");
    CHECK_STR(json_str(json_get(json_at(f, 2), "repo")), "other/a");
    CHECK_STR(json_str(json_get(json_at(f, 3), "repo")), "other/z");
    CHECK_STR(json_str(json_get(json_at(f, 4), "repo")), "");
    const Json *indices = json_get(json_at(f, 0), "indices");
    CHECK_INT(json_count(indices), 2); CHECK_INT(json_int_or(json_at(indices, 0), -1), 2); CHECK_INT(json_int_or(json_at(indices, 1), -1), 5);
    json_free(f);
    const char *queries[] = { " FIRST ", "Z/REPO", "deploy", "STAGING", "prod.EXAMPLE", "unassigned", "missing" };
    int counts[] = {2,2,1,1,1,1,0};
    for (size_t i = 0; i < 7; i++) {
        f = ssh_server_folders(rows, projects, n, queries[i]);
        CHECK_INT(json_count(f), counts[i] ? 1 : 0);
        if (counts[i]) CHECK_INT(json_count(json_get(json_at(f, 0), "indices")), counts[i]);
        json_free(f);
    }
    f = ssh_server_folders(NULL, NULL, 0, NULL); CHECK_INT(json_count(f), 0); json_free(f);
    projects_free(projects, n); json_free(rows);
}
static void folds_and_identity(void) {
    Json *folds = json_object();
    CHECK(ssh_server_folder_open(folds, "o/r", ""));
    ssh_server_folder_toggle(folds, "o/r", ""); CHECK(!ssh_server_folder_open(folds, "o/r", "  "));
    CHECK(ssh_server_folder_open(folds, "o/r", "match"));
    ssh_server_folder_toggle(folds, "o/r", "match"); CHECK(!ssh_server_folder_open(folds, "o/r", ""));
    ssh_server_folder_toggle(folds, "o/r", ""); CHECK(ssh_server_folder_open(folds, "o/r", "")); json_free(folds);
    Json *row = json_parsez("{\"id\":1729000000001,\"repo\":\"one\",\"host\":\"example\",\"username\":\"deploy\"}");
    CHECK_OWNED_STR(ssh_server_key(row), "ssh:1729000000001");
    json_set_str(row, "repo", "two"); CHECK_OWNED_STR(ssh_server_key(row), "ssh:1729000000001");
    CHECK_OWNED_STR(ssh_server_name(row), "deploy@example");
    json_set_str(row, "label", "Production"); CHECK_OWNED_STR(ssh_server_name(row), "Production");
    json_free(row);
}
void core_ssh_servers_tests(void) {
    test_run("SSH global folders preserve registry and sidebar order and filter all search fields", folders_and_filter);
    test_run("SSH search overrides folds without mutating them and identity follows registry id", folds_and_identity);
}
