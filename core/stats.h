#ifndef BRIAREUS_STATS_H
#define BRIAREUS_STATS_H
#include "json.h"

// Optional numeric metrics preserve a measured zero separately from unavailable data.
typedef struct { double value; bool present; } StatNumber;
typedef struct { StatNumber cpu, memory, processes; } ProcessStats;
typedef struct { StatNumber cpus, memory, interval_ms; } StatsHost;
enum { STATS_AGENT, STATS_SERVE, STATS_BROWSER, STATS_ROLES };
typedef struct { double at_ms; ProcessStats figures; } StatsSample;
typedef struct {
    bool open, roles[STATS_ROLES];
    StatsHost host;
    ProcessStats total, role[STATS_ROLES];
    StatNumber at_ms, disk_bytes, disk_at_ms;
    StatsSample *history; size_t history_count;
} SessionStats;
// Parses the session endpoint only. Rejects wrong identities/envelopes, accepts partial metrics.
// On success owns history; free with session_stats_free. Failure leaves a zeroed output.
bool session_stats_parse(const Json *answer, const char *session_id, SessionStats *out);
void session_stats_free(SessionStats *stats);
const char *stats_role_label(int role);
StatNumber stats_share(StatNumber part, StatNumber whole);
typedef enum { STAT_MUTED, STAT_OK, STAT_WARN, STAT_DANGER } StatTone;
StatTone stats_tone(StatNumber fraction);
// All returned strings are owned. Binary byte units match Swift Resources.
char *stats_bytes(StatNumber number);
char *stats_percent(StatNumber number);
char *stats_processes(StatNumber number, bool label);
char *stats_time(StatNumber epoch_ms);
// Server cadence, default 5s, minimum 2s; capped at the timer's signed-int limit.
int stats_poll_ms(const StatsHost *host);
// An absolute deadline in epoch seconds, never shortened, with bounded retry/backoff arithmetic.
double stats_retry_deadline(double now, double previous, int failures, double retry_after);

typedef struct { const char *label; char *value, *detail; StatNumber fraction; } StatTile;
void session_stats_tiles(const SessionStats *stats, StatTile out[4]);
void stats_tiles_free(StatTile *tiles, size_t count);
StatNumber stats_cpu_share(const ProcessStats *figures, const StatsHost *host);
typedef enum { STATS_PERCENT, STATS_BYTES } StatsUnit;
typedef struct { double at_ms; StatNumber number; } StatsPoint;
typedef struct {
    const char *title, *label; StatsUnit unit; int slot;
    StatsPoint *points; size_t count, valid_count;
    double first_ms, last_ms, maximum; StatNumber latest;
} StatsSeries;
// Points keep missing metrics as gaps. CPU is in one-core percent, not machine percent.
StatsSeries session_stats_series(const SessionStats *stats, StatsUnit unit);
void stats_series_free(StatsSeries *series);
char *stats_axis(StatsUnit unit, StatNumber value);
#endif
