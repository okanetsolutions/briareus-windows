#ifndef BRIAREUS_USAGE_H
#define BRIAREUS_USAGE_H
#include "json.h"

enum { USAGE_CUSTOM = 6, USAGE_PERIOD_COUNT = 7, USAGE_MAX_DAYS = 366 };
typedef struct { char from[11], to[11]; } UsageRange;
typedef struct { int period; UsageRange range; } UsageWindow;
extern const char *const usage_period_ids[USAGE_PERIOD_COUNT];
extern const char *const usage_period_labels[USAGE_PERIOD_COUNT];

/// Strict Gregorian days (0100..9999, matching the server's four-digit Date constructor).
/// Orders the days and accepts at most 366 inclusive days; failure leaves out unchanged.
bool usage_range_make(const char *a, const char *b, UsageRange *out);
/// Six calendar days before today through today; no epoch or timezone conversion.
bool usage_range_last_week(const char *today, UsageRange *out);
bool usage_window_set_period(UsageWindow *window, int period);
/// NULL (picker cancellation) or invalid input leaves the window unchanged.
bool usage_window_set_range(UsageWindow *window, const UsageRange *range);
/// Owned label and arguments; days travel unchanged for interpretation in the server's zone.
char *usage_window_label(const UsageWindow *window);
Json *usage_window_args(const UsageWindow *window);
/// Borrowed preset cache key, or NULL for custom/filtered queries; uses the answered request.
const char *usage_query_cache_key(const Json *args);

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
