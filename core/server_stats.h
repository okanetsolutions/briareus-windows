#ifndef BRIAREUS_SERVER_STATS_H
#define BRIAREUS_SERVER_STATS_H
#include "stats.h"
#include "models.h"
typedef struct { char *label; StatNumber total, free_bytes, used; } StatsDisk;
typedef struct {
    StatsHost host;
    StatNumber cpu, free_memory, uptime, load[3];
    ProcessStats process, sessions;
    StatNumber heap_used, heap_total, process_uptime, open_sessions, disk_bytes;
    char *node;
    StatsDisk *disks; size_t disk_count;
    // Four lines: machine CPU, process CPU scaled to machine, used memory, process memory.
    StatsSeries lines[4];
    StatNumber at_ms;
} ServerStats;
typedef struct {
    char *id; Session record;
    ProcessStats figures; StatNumber at_ms, disk_bytes, disk_at_ms;
} StatsSession;
typedef struct { StatsHost host; StatsSession *items; size_t count; StatNumber at_ms; } OpenStats;
// Owned parses; strict endpoint envelopes, partial numeric metrics remain unavailable.
bool server_stats_parse(const Json *j, ServerStats *out);
void server_stats_free(ServerStats *s);
bool open_stats_parse(const Json *j, const Session *known, size_t count, OpenStats *out);
void open_stats_free(OpenStats *s);
void server_stats_tiles(const ServerStats *s, StatTile out[6]);
StatTile server_stats_disk_tile(const StatsDisk *disk);
// Owned copy for the shared chart renderer.
StatsSeries stats_series_copy(const StatsSeries *s);
char *stats_uptime(StatNumber seconds);
char *stats_age(StatNumber at_ms, double now_seconds);
#endif
