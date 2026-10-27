#include "webhook.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>

static void caps_load(void) {
    Json *hook = json_parsez("{\"perHour\":120,\"maxTurns\":0}");
    CHECK_OWNED_STR(webhook_field_text(hook, WEBHOOK_PER_HOUR), "120");
    CHECK_OWNED_STR(webhook_field_text(hook, WEBHOOK_MAX_TURNS), "0");
    json_free(hook);
    hook = json_object();
    CHECK_OWNED_STR(webhook_field_text(hook, WEBHOOK_PER_HOUR), "30");
    CHECK_OWNED_STR(webhook_field_text(hook, WEBHOOK_MAX_TURNS), "10");
    json_free(hook);
}
static void save_caps(void) {
    const char *caps[] = {" 30 ", "0"}; WebhookField invalid;
    Json *body = webhook_save(true, false, true, caps, &invalid);
    Json *expected = json_parsez("{\"armed\":true,\"instructions\":false,\"sshUnattended\":true,\"perHour\":30,\"maxTurns\":0}");
    CHECK(body != NULL); CHECK(json_equal(body, expected));
    char *wire = json_serialize(body, false);
    Json *again = json_parsez(wire);
    CHECK_INT(json_int_or(json_get(again, "maxTurns"), -1), 0);
    free(wire); json_free(again); json_free(body); json_free(expected);
    const char *hours[] = {"1", "600"};
    const char *turns[] = {"1", "1000"};
    for (int i = 0; i < 2; i++) {
        caps[0] = hours[i]; caps[1] = turns[i];
        body = webhook_save(false, true, false, caps, &invalid);
        CHECK(body != NULL);
        CHECK_INT(json_int_or(json_get(body, "perHour"), -1), i ? 600 : 1);
        CHECK_INT(json_int_or(json_get(body, "maxTurns"), -1), i ? 1000 : 1);
        json_free(body);
    }
}
static void invalid_caps(void) {
    const char *bad[] = {NULL, "", " ", "-1", "1.5", "1e2", "NaN", "2x", "999999999999999999999999"};
    WebhookField invalid;
    for (int f = 0; f < WEBHOOK_FIELD_COUNT; f++) {
        for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
            const char *caps[] = {"30", "10"}; caps[f] = bad[i];
            CHECK(webhook_save(false, false, false, caps, &invalid) == NULL);
            CHECK_INT(invalid, f);
        }
    }
    const char *caps[] = {"0", "10"};
    CHECK(webhook_save(false, false, false, caps, &invalid) == NULL); CHECK_INT(invalid, WEBHOOK_PER_HOUR);
    caps[0] = "601";
    CHECK(webhook_save(false, false, false, caps, &invalid) == NULL); CHECK_INT(invalid, WEBHOOK_PER_HOUR);
    caps[0] = "30"; caps[1] = "1001";
    CHECK(webhook_save(false, false, false, caps, &invalid) == NULL); CHECK_INT(invalid, WEBHOOK_MAX_TURNS);
}
void webhook_tests(void) {
    test_run("webhook caps preserve zero and default missing fields", caps_load);
    test_run("webhook save serializes unlimited and positive boundaries", save_caps);
    test_run("webhook save rejects malformed and out of range caps", invalid_caps);
}
