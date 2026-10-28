#include "test.h"
#include "suites.h"
#include "ssh_settings.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

static void capability_matrix(void) {
    for (int mask = 0; mask < 4; mask++) for (int stored = 0; stored < 2; stored++) {
        Json *row = json_object(), *rows = json_array();
        for (int f = 0; f < 2; f++) if (mask & (1 << f)) json_set_bool(row, SSH_SECRET_FLAGS[f], stored != 0);
        json_array_push(rows, json_clone(row));
        SshSecrets s; ssh_secrets_init(&s, row, rows, false);
        for (int f = 0; f < 2; f++) {
            CHECK_INT(s.fields[f].offered, (mask & (1 << f)) != 0);
            CHECK_INT(s.fields[f].stored, stored && (mask & (1 << f)));
        }
        ssh_secrets_free(&s); ssh_secrets_init(&s, NULL, rows, true);
        for (int f = 0; f < 2; f++) { CHECK_INT(s.fields[f].offered, (mask & (1 << f)) != 0); CHECK(!s.fields[f].stored); }
        ssh_secrets_free(&s); json_free(row); json_free(rows);
    }
    Json *empty = json_array(); SshSecrets s;
    ssh_secrets_init(&s, NULL, empty, true); CHECK(s.fields[0].offered); CHECK(s.fields[1].offered); ssh_secrets_free(&s);
    ssh_secrets_init(&s, NULL, NULL, true); CHECK(!s.fields[0].offered); CHECK(!s.fields[1].offered); ssh_secrets_free(&s);
    Json *row = json_parsez("{\"hasPassword\":null,\"hasSudoPassword\":false}");
    ssh_secrets_init(&s, row, NULL, false); CHECK(s.fields[0].offered); CHECK(!s.fields[0].stored); ssh_secrets_free(&s);
    json_free(row); json_free(empty);
}
static void modes_and_clone(void) {
    Json *row = json_parsez("{\"hasPassword\":true,\"hasSudoPassword\":false}"); SshSecrets s;
    ssh_secrets_init(&s, row, NULL, false); Json *body = json_clone(row); char *error = NULL;
    CHECK(ssh_secrets_body(&s, body, &error)); CHECK(json_is_null(json_get(body, "password"))); CHECK(json_is_null(json_get(body, "hasPassword")));
    CHECK(!ssh_secrets_changed(&s));
    ssh_secrets_toggle(&s, 0); CHECK(ssh_secrets_changed(&s)); CHECK(ssh_secrets_body(&s, body, &error)); CHECK_STR(json_str(json_get(body, "password")), "");
    ssh_secrets_set(&s, 0, "fixture draft", 13); CHECK(!s.fields[0].remove);
    CHECK(ssh_secrets_body(&s, body, &error)); CHECK_STR(json_str(json_get(body, "password")), "fixture draft");
    SshSecrets copy; ssh_secrets_clone(&copy, &s); CHECK(copy.fields[0].offered); CHECK(!copy.fields[0].stored); CHECK_STR(copy.fields[0].text, "fixture draft"); ssh_secrets_free(&copy);
    ssh_secrets_toggle(&s, 0); ssh_secrets_clone(&copy, &s); CHECK(!copy.fields[0].remove); CHECK(!copy.fields[0].bytes); ssh_secrets_free(&copy);
    ssh_secrets_toggle(&s, 0); CHECK(!ssh_secrets_changed(&s));
    s.fields[1].offered = false; ssh_secrets_set(&s, 1, "fixture draft", 13); ssh_secrets_toggle(&s, 1);
    CHECK(ssh_secrets_body(&s, body, &error)); CHECK(json_is_null(json_get(body, "sudoPassword"))); CHECK(ssh_secrets_changed(&s));
    ssh_secrets_free(&s); ssh_secrets_scrub(body); json_free(body); json_free(row); free(error);
}
static void validation(void) {
    Json *row = json_parsez("{\"hasPassword\":false,\"hasSudoPassword\":false}");
    SshSecrets s; ssh_secrets_init(&s, row, NULL, false);
    char text[1025]; memset(text, 'x', sizeof text);
    // 511 two-byte code points plus one ASCII byte is exactly the byte limit.
    for (int i = 0; i < 1022; i += 2) { memcpy(text + i, "\xc3\xa9", 2); }
    for (int f = 0; f < 2; f++) for (int mode = 0; mode < 6; mode++) {
        size_t bytes = mode == 0 ? 1023 : mode == 1 ? 1024 : 3;
        const char *v = mode < 2 ? text : mode == 2 ? "a\rb" : mode == 3 ? "a\nb" : mode == 4 ? "a\0b" : " x ";
        ssh_secrets_set(&s, f, v, bytes); Json *body = json_object(); char *error = NULL;
        CHECK_INT(ssh_secrets_body(&s, body, &error), mode == 0 || mode == 5);
        if (error) CHECK(strstr(error, "fixture") == NULL);
        free(error); ssh_secrets_scrub(body); json_free(body); ssh_secrets_set(&s, f, "", 0);
    }
    ssh_secrets_free(&s); json_free(row);
}
static void acknowledgement(void) {
    Json *row = json_parsez("{\"hasPassword\":false,\"hasSudoPassword\":false}");
    SshSecrets s; ssh_secrets_init(&s, row, NULL, false);
    ssh_secrets_set(&s, 0, "fixture draft", 13); ssh_secrets_set(&s, 1, "fixture sudo", 12);
    unsigned long long revisions[2] = {s.fields[0].revision, s.fields[1].revision};
    Json *body = json_object(); char *error = NULL; CHECK(ssh_secrets_body(&s, body, &error));
    CHECK(!ssh_secrets_saved(&s, row, body, revisions)); CHECK(s.fields[0].bytes); CHECK(s.fields[1].bytes);
    ssh_secrets_set(&s, 0, "newer draft", 11); json_set_bool(row, "hasPassword", true); json_set_bool(row, "hasSudoPassword", true);
    CHECK(ssh_secrets_saved(&s, row, body, revisions)); CHECK_STR(s.fields[0].text, "newer draft"); CHECK(!s.fields[1].bytes); CHECK(s.fields[0].stored);
    json_object_remove(row, "hasPassword"); CHECK(!ssh_secrets_saved(&s, row, body, revisions)); CHECK(!s.fields[0].offered); CHECK(!s.fields[0].stored); CHECK(s.fields[0].bytes);
    Json *unrelated = json_object(); CHECK(ssh_secrets_body(&s, unrelated, &error));
    CHECK(json_is_null(json_get(unrelated, "password")));
    revisions[0] = s.fields[0].revision;
    CHECK(ssh_secrets_saved(&s, row, unrelated, revisions)); CHECK(s.fields[0].bytes); json_free(unrelated);
    ssh_secrets_scrub(body); json_free(body); json_free(row); ssh_secrets_free(&s); free(error);
}
void core_ssh_settings_tests(void) {
    test_run("SSH per-field capabilities and new defaults", capability_matrix);
    test_run("SSH keep replace remove typed precedence and clone", modes_and_clone);
    test_run("SSH UTF8 byte boundaries and invalid characters", validation);
    test_run("SSH authoritative flags and stale draft acknowledgement", acknowledgement);
}
