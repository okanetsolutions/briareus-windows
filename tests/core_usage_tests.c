#include "usage.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>
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

static void test_calendar_ranges(void) {
    static const struct { const char *a, *b, *from, *to; } cases[] = {
        { "2026-08-03", "2026-08-09", "2026-08-03", "2026-08-09" },
        { "2026-08-09", "2026-08-03", "2026-08-03", "2026-08-09" },
        { "2024-02-29", "2024-02-29", "2024-02-29", "2024-02-29" },
        { "2024-01-01", "2024-12-31", "2024-01-01", "2024-12-31" },
        { "2026-01-01", "2027-01-01", "2026-01-01", "2027-01-01" },
        { "2026-12-31", "2027-01-02", "2026-12-31", "2027-01-02" },
        { "2026-03-07", "2026-03-09", "2026-03-07", "2026-03-09" },
        { "2026-10-31", "2026-11-02", "2026-10-31", "2026-11-02" },
        { "2000-02-29", "2000-03-01", "2000-02-29", "2000-03-01" },
        { "0100-01-01", "0100-01-01", "0100-01-01", "0100-01-01" },
        { "9999-12-31", "9999-12-31", "9999-12-31", "9999-12-31" }
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        UsageRange r;
        CHECK(usage_range_make(cases[i].a, cases[i].b, &r));
        CHECK_STR(r.from, cases[i].from); CHECK_STR(r.to, cases[i].to);
    }
    static const char *const invalid[] = {
        "2023-02-29", "1900-02-29", "2026-04-31", "2026-00-01", "2026-13-01", "2026-01-00",
        "2026-01-32", "2026-1-01", "2026-01-1", "2026-01-01x", "2026-01-01T00:00:00Z",
        "0000-01-01", "0099-12-31", "10000-01-01", "-001-01-01", "202a-01-01", "", NULL
    };
    UsageRange r = { "2026-01-01", "2026-01-02" };
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        CHECK(!usage_range_make(invalid[i], "2026-01-01", &r));
        CHECK(!usage_range_make("2026-01-01", invalid[i], &r));
        CHECK_STR(r.from, "2026-01-01"); CHECK_STR(r.to, "2026-01-02");
    }
    CHECK(!usage_range_make("2024-01-01", "2025-01-01", &r));
    CHECK(!usage_range_make("2027-01-02", "2026-01-01", &r));
    CHECK(usage_range_last_week("2024-03-02", &r)); CHECK_STR(r.from, "2024-02-25"); CHECK_STR(r.to, "2024-03-02");
    CHECK(usage_range_last_week("2026-01-02", &r)); CHECK_STR(r.from, "2025-12-27");
    CHECK(usage_range_last_week("2026-03-10", &r)); CHECK_STR(r.from, "2026-03-04");
    CHECK(!usage_range_last_week("bad", &r)); CHECK(!usage_range_last_week("0100-01-01", &r));
}

static void test_custom_query(void) {
    UsageWindow w = {0};
    Json *args = usage_window_args(&w); CHECK_STR(usage_query_cache_key(args), "usage:month"); json_free(args);
    UsageRange r = { "2026-08-09", "2026-08-03" };
    CHECK(usage_window_set_range(&w, &r)); CHECK_INT(w.period, USAGE_CUSTOM);
    CHECK_STR(w.range.from, "2026-08-03"); CHECK_STR(w.range.to, "2026-08-09");
    args = usage_window_args(&w);
    CHECK_STR(json_str(json_get(args, "period")), "custom");
    CHECK_STR(json_str(json_get(args, "from")), "2026-08-03"); CHECK_STR(json_str(json_get(args, "to")), "2026-08-09");
    CHECK(usage_query_cache_key(args) == NULL); json_free(args);
    char *label = usage_window_label(&w); CHECK(strstr(label, "2026-08-03") != NULL); CHECK(strstr(label, "2026-08-09") != NULL); free(label);
    CHECK(!usage_window_set_range(&w, NULL)); CHECK(!usage_window_set_period(&w, -1));
    CHECK(!usage_window_set_period(&w, USAGE_CUSTOM)); CHECK(!usage_window_set_period(&w, USAGE_PERIOD_COUNT));
    r = (UsageRange){ "bad", "2026-08-03" }; CHECK(!usage_window_set_range(&w, &r)); CHECK_INT(w.period, USAGE_CUSTOM);
    for (int i = 0; i < USAGE_CUSTOM; i++) {
        CHECK(usage_window_set_period(&w, i)); CHECK(!usage_window_set_period(&w, i));
        args = usage_window_args(&w); CHECK_INT(json_count(args), 1); CHECK(usage_query_cache_key(args) != NULL);
        CHECK_STR(json_str(json_get(args, "period")), usage_period_ids[i]);
        CHECK_STR(w.range.from, "2026-08-03");
        label = usage_window_label(&w); CHECK_STR(label, usage_period_labels[i]); free(label);
        json_set_str(args, "account", "7"); CHECK(usage_query_cache_key(args) == NULL); json_free(args);
    }
    r = w.range; CHECK(usage_window_set_range(&w, &r)); CHECK_STR(w.range.to, "2026-08-09");
    r = (UsageRange){ "2024-02-29", "2024-02-29" }; CHECK(usage_window_set_range(&w, &r));
    label = usage_window_label(&w); CHECK_STR(label, "2024-02-29"); free(label);
    CHECK(usage_query_cache_key(NULL) == NULL);
    args = json_parsez("{\"period\":\"invalid\"}"); CHECK(usage_query_cache_key(args) == NULL); json_free(args);
}

void usage_tests(void) {
    test_run("usage custom ranges count strict Gregorian calendar days including DST and leap boundaries", test_calendar_ranges);
    test_run("usage custom queries retain dates while isolating preset caches and cancellation", test_custom_query);
    test_run("usage comparison preserves its window and elapsed-time marker", test_comparison_presence_and_metadata);
    test_run("usage comparison needs numeric values in both windows", test_comparison_missing_numbers);
    test_run("usage comparison distinguishes unchanged zero from new usage", test_zero_previous_usage);
    test_run("usage comparison computes increases, decreases and unchanged totals", test_percentage_changes);
    test_run("usage comparison marks partial pricing in either window", test_partial_pricing_either_window);
    test_run("usage pricing coverage rounds and handles empty or missing counts", test_pricing_coverage);
}
