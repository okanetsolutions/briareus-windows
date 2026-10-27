#include "stats.h"
#include "str.h"
#include <stdlib.h>

StatNumber stats_cpu_share(const ProcessStats *figures, const StatsHost *host) {
    StatNumber whole = host->cpus; whole.value *= 100;
    return stats_share(figures->cpu, whole);
}
void session_stats_tiles(const SessionStats *s, StatTile out[4]) {
    char *cores = stats_processes(s->host.cpus, false), *memory = stats_bytes(s->host.memory);
    Str roles; str_init(&roles);
    for (int r = 0; r < STATS_ROLES; r++) if (s->roles[r]) str_appendf(&roles, "%s%s", roles.len ? ", " : "", stats_role_label(r));
    out[0] = (StatTile){"CPU", stats_percent(s->total.cpu), xstrfmt("of one core · %s on the machine", cores), stats_cpu_share(&s->total, &s->host)};
    out[1] = (StatTile){"Memory", stats_bytes(s->total.memory), xstrfmt("of %s", memory), stats_share(s->total.memory, s->host.memory)};
    out[2] = (StatTile){"Processes", stats_processes(s->total.processes, false), str_detach(&roles), {0}};
    out[3] = (StatTile){"Workspace", stats_bytes(s->disk_bytes), xstrdup(s->disk_bytes.present ? "on disk" : "not measured yet"), {0}};
    free(cores); free(memory);
}
void stats_tiles_free(StatTile *tiles, size_t count) {
    for (size_t i = 0; i < count; i++) { free(tiles[i].value); free(tiles[i].detail); }
}
StatsSeries session_stats_series(const SessionStats *s, StatsUnit unit) {
    StatsSeries line = {.title = unit == STATS_PERCENT ? "CPU" : "Memory", .label = "Session", .unit = unit, .slot = unit == STATS_PERCENT ? 0 : 1};
    line.count = s->history_count;
    if (!line.count) return line;
    line.points = xcalloc(line.count, sizeof *line.points);
    line.first_ms = s->history[0].at_ms; line.last_ms = s->history[line.count - 1].at_ms;
    for (size_t i = 0; i < line.count; i++) {
        StatNumber n = unit == STATS_PERCENT ? s->history[i].figures.cpu : s->history[i].figures.memory;
        line.points[i] = (StatsPoint){s->history[i].at_ms, n};
        if (!n.present) continue;
        line.valid_count++; line.latest = n;
        if (n.value > line.maximum) line.maximum = n.value;
    }
    return line;
}
void stats_series_free(StatsSeries *series) { free(series->points); *series = (StatsSeries){0}; }
char *stats_axis(StatsUnit unit, StatNumber value) { return unit == STATS_PERCENT ? stats_percent(value) : stats_bytes(value); }
