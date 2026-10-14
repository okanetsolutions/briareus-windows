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
