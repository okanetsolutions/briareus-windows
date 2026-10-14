#ifndef BRIAREUS_USAGE_H
#define BRIAREUS_USAGE_H
#include "json.h"

typedef enum {
    USAGE_CHANGE_UNAVAILABLE,
    USAGE_CHANGE_UNCHANGED,
    USAGE_CHANGE_NEW,
    USAGE_CHANGE_PERCENT
} UsageChangeKind;

typedef struct {
    UsageChangeKind kind;
    double percent;
} UsageChange;

typedef enum { USAGE_COST, USAGE_TOKENS, USAGE_SESSIONS, USAGE_METRIC_COUNT } UsageMetric;

typedef struct {
    bool present, partial_elapsed, partial_cost;
    double from_ms, to_ms;
    UsageChange changes[USAGE_METRIC_COUNT];
} UsageComparison;

typedef struct {
    int turns, priced_turns, percent;
} UsagePricing;

/// Reads the previous window and changes from a usage ledger payload; returned values own no memory.
UsageComparison usage_comparison(const Json *usage);
/// Missing or non-integer turn counts default to zero; coverage rounds to the nearest whole percent.
UsagePricing usage_pricing(const Json *usage);

#endif
