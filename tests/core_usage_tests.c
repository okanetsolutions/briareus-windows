#include "usage.h"
#include "suites.h"
#include "test.h"

static void test_comparison_presence_and_metadata(void) {
    CHECK(!usage_comparison(NULL).present);
    Json *usage = json_parsez("{\"comparison\":[]}");
    CHECK(!usage_comparison(usage).present);
    json_free(usage);
    usage = json_parsez("{\"comparison\":{\"from\":123000,\"to\":456000,\"partial\":true}}");
    UsageComparison c = usage_comparison(usage);
    CHECK(c.present); CHECK(c.partial_elapsed); CHECK(!c.partial_cost);
    CHECK(c.from_ms == 123000); CHECK(c.to_ms == 456000);
    json_free(usage);
    usage = json_parsez("{\"comparison\":{\"from\":\"bad\",\"partial\":\"true\"}}");
    c = usage_comparison(usage);
    CHECK(c.present); CHECK(!c.partial_elapsed); CHECK(c.from_ms == 0); CHECK(c.to_ms == 0);
    json_free(usage);
}

static void test_comparison_missing_numbers(void) {
    static const char *const payloads[] = {
        "{\"comparison\":{}}",
        "{\"costUsd\":null,\"totalTokens\":\"4\",\"sessions\":true,\"comparison\":{\"costUsd\":1,\"totalTokens\":2,\"sessions\":3}}",
        "{\"costUsd\":1,\"totalTokens\":2,\"sessions\":3,\"comparison\":{\"costUsd\":false,\"totalTokens\":null,\"sessions\":\"3\"}}"
    };
    for (size_t i = 0; i < sizeof payloads / sizeof *payloads; i++) {
        Json *usage = json_parsez(payloads[i]);
        UsageComparison c = usage_comparison(usage);
        for (int k = 0; k < USAGE_METRIC_COUNT; k++) CHECK_INT(c.changes[k].kind, USAGE_CHANGE_UNAVAILABLE);
        json_free(usage);
    }
}

static void test_zero_previous_usage(void) {
    Json *usage = json_parsez("{\"costUsd\":0,\"totalTokens\":120,\"sessions\":1,\"comparison\":{\"costUsd\":0,\"totalTokens\":0,\"sessions\":0}}");
    UsageComparison c = usage_comparison(usage);
    CHECK_INT(c.changes[USAGE_COST].kind, USAGE_CHANGE_UNCHANGED);
    CHECK_INT(c.changes[USAGE_TOKENS].kind, USAGE_CHANGE_NEW);
    CHECK_INT(c.changes[USAGE_SESSIONS].kind, USAGE_CHANGE_NEW);
    CHECK(c.changes[USAGE_TOKENS].percent == 0);
    json_free(usage);
}

static void test_percentage_changes(void) {
    Json *usage = json_parsez("{\"costUsd\":15,\"totalTokens\":60,\"sessions\":3,\"comparison\":{\"costUsd\":10,\"totalTokens\":80,\"sessions\":3}}");
    UsageComparison c = usage_comparison(usage);
    for (int k = 0; k < USAGE_METRIC_COUNT; k++) CHECK_INT(c.changes[k].kind, USAGE_CHANGE_PERCENT);
    CHECK(c.changes[USAGE_COST].percent == 50);
    CHECK(c.changes[USAGE_TOKENS].percent == -25);
    CHECK(c.changes[USAGE_SESSIONS].percent == 0);
    json_set_num(usage, "sessions", 0);
    c = usage_comparison(usage);
    CHECK(c.changes[USAGE_SESSIONS].percent == -100);
    json_free(usage);
}

static void test_partial_pricing_either_window(void) {
    Json *usage = json_parsez("{\"comparison\":{}}");
    CHECK(!usage_comparison(usage).partial_cost);
    json_set_num(usage, "unpricedTurns", 1);
    CHECK(usage_comparison(usage).partial_cost);
    json_set_num(usage, "unpricedTurns", 0);
    Json *previous = json_object(); json_set_num(previous, "unpricedTurns", 2);
    json_object_set(usage, "comparison", previous);
    CHECK(usage_comparison(usage).partial_cost);
    json_set_num(previous, "unpricedTurns", 0);
    CHECK(!usage_comparison(usage).partial_cost);
    json_object_set(usage, "unpricedTurns", json_string("1"));
    CHECK(!usage_comparison(usage).partial_cost);
    json_free(usage);
}

static void test_pricing_coverage(void) {
    static const struct { const char *payload; int turns, priced, percent; } cases[] = {
        { "{}", 0, 0, 0 },
        { "{\"turns\":0}", 0, 0, 0 },
        { "{\"turns\":3,\"unpricedTurns\":1}", 3, 2, 67 },
        { "{\"turns\":3,\"unpricedTurns\":2}", 3, 1, 33 },
        { "{\"turns\":8,\"unpricedTurns\":7}", 8, 1, 13 },
        { "{\"turns\":4,\"unpricedTurns\":4}", 4, 0, 0 },
        { "{\"turns\":4}", 4, 4, 100 },
        { "{\"turns\":\"4\",\"unpricedTurns\":null}", 0, 0, 0 },
        { "{\"turns\":4.5,\"unpricedTurns\":1.5}", 0, 0, 0 }
    };
    UsagePricing missing = usage_pricing(NULL);
    CHECK_INT(missing.turns, 0); CHECK_INT(missing.percent, 0);
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        Json *usage = json_parsez(cases[i].payload);
        UsagePricing pricing = usage_pricing(usage);
        CHECK_INT(pricing.turns, cases[i].turns);
        CHECK_INT(pricing.priced_turns, cases[i].priced);
        CHECK_INT(pricing.percent, cases[i].percent);
        json_free(usage);
    }
}

void usage_tests(void) {
    test_run("usage comparison preserves its window and elapsed-time marker", test_comparison_presence_and_metadata);
    test_run("usage comparison needs numeric values in both windows", test_comparison_missing_numbers);
    test_run("usage comparison distinguishes unchanged zero from new usage", test_zero_previous_usage);
    test_run("usage comparison computes increases, decreases and unchanged totals", test_percentage_changes);
    test_run("usage comparison marks partial pricing in either window", test_partial_pricing_either_window);
    test_run("usage pricing coverage rounds and handles empty or missing counts", test_pricing_coverage);
}
