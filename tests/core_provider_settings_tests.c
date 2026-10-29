#include "provider_settings.h"
#include "test.h"
#include "suites.h"

static Json *catalog(const char *ids) {
    Json *doc = json_parsez("{\"components\":{\"schemas\":{\"Provider\":{\"properties\":{\"binary\":{}}}}}}");
    Json *binary = (Json *)json_get(json_get(json_get(json_get(json_get(doc, "components"), "schemas"), "Provider"), "properties"), "binary");
    json_object_set(binary, "enum", json_parsez(ids));
    return doc;
}

static void binary_ids(void) {
    Json *doc = catalog("[\"claude\",\"codex\",\"grok\",\"opencode\",\"dsh\",\"qwen\"]");
    Json *ids = provider_settings_binary_ids(doc); json_free(doc);
    CHECK_INT(json_count(ids), 6);
    CHECK_STR(json_str(json_at(ids, 0)), "claude");
    CHECK_STR(json_str(json_at(ids, 4)), "dsh"); CHECK_STR(json_str(json_at(ids, 5)), "qwen");
    json_free(ids);
    doc = catalog("[\"future-cli\",\"qwen\"]"); ids = provider_settings_binary_ids(doc);
    CHECK_STR(json_str(json_at(ids, 0)), "future-cli"); json_free(ids); json_free(doc);
    const char *bad[] = { "[]", "null", "{}", "[\"\"]", "[\"codex\",null]", "[\"codex\",4]", "[\"qwen\",\"qwen\"]" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        doc = catalog(bad[i]); CHECK(provider_settings_binary_ids(doc) == NULL); json_free(doc);
    }
    doc = catalog("[]");
    Json *many = (Json *)json_get(json_get(json_get(json_get(json_get(json_get(doc, "components"), "schemas"), "Provider"), "properties"), "binary"), "enum");
    for (int i = 0; i < 129; i++) json_array_push(many, json_number(i));
    CHECK(provider_settings_binary_ids(doc) == NULL); json_free(doc);
    CHECK(provider_settings_binary_ids(NULL) == NULL);
    doc = json_parsez("{\"providers\":[{\"binary\":\"claude\"}],\"defaults\":{\"binary\":\"claude\"}}");
    CHECK(provider_settings_binary_ids(doc) == NULL); json_free(doc);
}

void provider_settings_tests(void) { test_run("provider supported IDs use validated OpenAPI enum", binary_ids); }
