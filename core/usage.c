#include "str.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "usage.h"

static UsageChange change(const Json *current, const Json *previous, const char *key) {
    double now, old;
    if (!json_num(json_get(previous, key), &old) || !json_num(json_get(current, key), &now))
        return (UsageChange){ USAGE_CHANGE_UNAVAILABLE, 0 };
    if (!old) return (UsageChange){ now ? USAGE_CHANGE_NEW : USAGE_CHANGE_UNCHANGED, 0 };
    return (UsageChange){ USAGE_CHANGE_PERCENT, (now - old) / old * 100 };
}

UsageComparison usage_comparison(const Json *usage) {
    UsageComparison out = {0};
    const Json *previous = json_get(usage, "comparison");
    if (!json_is_object(previous)) return out;
    out.present = true;
    out.from_ms = json_num_or(json_get(previous, "from"), 0);
    out.to_ms = json_num_or(json_get(previous, "to"), 0);
    out.partial_elapsed = json_bool_is(json_get(previous, "partial"), true);
    out.partial_cost = json_int_or(json_get(usage, "unpricedTurns"), 0) != 0
        || json_int_or(json_get(previous, "unpricedTurns"), 0) != 0;
    out.changes[USAGE_COST] = change(usage, previous, "costUsd");
    out.changes[USAGE_TOKENS] = change(usage, previous, "totalTokens");
    out.changes[USAGE_SESSIONS] = change(usage, previous, "sessions");
    return out;
}

UsagePricing usage_pricing(const Json *usage) {
    UsagePricing out = {0};
    out.turns = json_int_or(json_get(usage, "turns"), 0);
    out.priced_turns = out.turns - json_int_or(json_get(usage, "unpricedTurns"), 0);
    out.percent = out.turns ? (int)(out.priced_turns * 100.0 / out.turns + 0.5) : 0;
    return out;
}

const char *const usage_period_ids[USAGE_PERIOD_COUNT] = { "month", "prev", "all", "today", "7d", "30d", "custom" };
const char *const usage_period_labels[USAGE_PERIOD_COUNT] = {
    "This month", "Last month", "All time", "Today", "Last 7 days", "Last 30 days", "Custom range\xE2\x80\xA6"
};

static int month_days(int y, int m) {
    static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return days[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}

static bool parse_day(const char *s, int *y, int *m, int *d) {
    if (!s || strlen(s) != 10 || s[4] != '-' || s[7] != '-') return false;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) continue;
        if (s[i] < '0' || s[i] > '9') return false;
    }
    *y = atoi(s); *m = atoi(s + 5); *d = atoi(s + 8);
    if (*y < 100 || *m < 1 || *m > 12 || *d < 1) return false;
    return *d <= month_days(*y, *m);
}

static int ordinal(int y, int m, int d) {
    int before = y - 1;
    int n = 365 * before + before / 4 - before / 100 + before / 400 + d;
    for (int i = 1; i < m; i++) n += month_days(y, i);
    return n;
}

bool usage_range_make(const char *a, const char *b, UsageRange *out) {
    int y, m, d;
    if (!parse_day(a, &y, &m, &d)) return false;
    int first = ordinal(y, m, d);
    if (!parse_day(b, &y, &m, &d)) return false;
    int last = ordinal(y, m, d);
    if (abs(last - first) >= USAGE_MAX_DAYS) return false;
    UsageRange result;
    memcpy(result.from, first <= last ? a : b, 11);
    memcpy(result.to, first <= last ? b : a, 11);
    *out = result;
    return true;
}

bool usage_range_last_week(const char *today, UsageRange *out) {
    int y, m, d;
    if (!parse_day(today, &y, &m, &d)) return false;
    for (int i = 0; i < 6; i++) {
        if (--d) continue;
        if (--m == 0) { m = 12; y--; }
        d = month_days(y, m);
    }
    char first[40];
    snprintf(first, sizeof first, "%04d-%02d-%02d", y, m, d);
    return usage_range_make(first, today, out);
}

bool usage_window_set_period(UsageWindow *window, int period) {
    if (period < 0 || period >= USAGE_CUSTOM || period == window->period) return false;
    window->period = period;
    return true;
}

bool usage_window_set_range(UsageWindow *window, const UsageRange *range) {
    UsageRange normalized;
    if (!range || !usage_range_make(range->from, range->to, &normalized)) return false;
    window->range = normalized; window->period = USAGE_CUSTOM;
    return true;
}

char *usage_window_label(const UsageWindow *window) {
    if (window->period != USAGE_CUSTOM) return xstrdup(usage_period_labels[window->period]);
    // Include years, also for cross-year ranges; no local midnight conversion is needed.
    if (str_eq(window->range.from, window->range.to)) return xstrdup(window->range.from);
    return xstrfmt("%s \xE2\x80\x93 %s", window->range.from, window->range.to);
}

Json *usage_window_args(const UsageWindow *window) {
    Json *args = json_object();
    json_set_str(args, "period", usage_period_ids[window->period]);
    if (window->period == USAGE_CUSTOM) {
        json_set_str(args, "from", window->range.from); json_set_str(args, "to", window->range.to);
    }
    return args;
}

const char *usage_query_cache_key(const Json *args) {
    static const char *const keys[] = { "usage:month", "usage:prev", "usage:all", "usage:today", "usage:7d", "usage:30d" };
    if (json_count(args) != 1) return NULL;
    const char *period = json_str(json_get(args, "period"));
    for (int i = 0; i < USAGE_CUSTOM; i++) if (str_eq(period, usage_period_ids[i])) return keys[i];
    return NULL;
}
