#include "server_stats.h"
#include "stats_internal.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

StatsHost stats_host(const Json *j) {
    return (StatsHost){stats_number(json_get(j, "cpus"), true), stats_number(json_get(j, "memoryBytes"), true), stats_number(json_get(j, "intervalMs"), true)};
}
StatNumber stats_used(StatNumber total, StatNumber free_bytes) {
    if (!total.present || !free_bytes.present || free_bytes.value > total.value) return (StatNumber){0};
    return (StatNumber){total.value - free_bytes.value, true};
}
static StatNumber metric(const Json *j, const char *key) { return stats_number(json_get(j, key), true); }
static int by_time(const void *a, const void *b) {
    double x = ((const StatsPoint *)a)->at_ms, y = ((const StatsPoint *)b)->at_ms;
    return (x > y) - (x < y);
}
static void history(ServerStats *s, const Json *samples) {
    const char *labels[] = {"Machine · all cores", "Briareus · all cores", "Machine in use", "Briareus"};
    const char *titles[] = {"CPU (% of machine)", "CPU (% of machine)", "Memory (binary bytes)", "Memory (binary bytes)"};
    for (size_t i = 0; i < json_count(samples); i++) {
        StatNumber at = stats_timestamp(json_get(json_at(samples, i), "at"));
        if (at.value > s->at_ms.value) s->at_ms = at;
    }
    for (int k = 0; k < 4; k++) {
        StatsSeries *line = &s->lines[k];
        *line = (StatsSeries){.title = titles[k], .label = labels[k], .unit = k < 2 ? STATS_PERCENT : STATS_BYTES, .slot = k % 2};
        line->gap_ms = stats_history_gap_ms(&s->host);
        for (size_t i = 0; i < json_count(samples); i++) {
            const Json *j = json_at(samples, i); StatNumber at = stats_timestamp(json_get(j, "at"));
            if (!at.present || at.value < s->at_ms.value - 3600000) continue;
            StatNumber n;
            if (k == 0) n = stats_number(json_get(j, "hostCpuPercent"), false);
            else if (k == 1) {
                n = stats_number(json_get(j, "cpuPercent"), false);
                if (!s->host.cpus.present || s->host.cpus.value <= 0) n.present = false;
                else n.value /= s->host.cpus.value;
            } else if (k == 2) n = stats_used(s->host.memory, metric(j, "freeMemoryBytes"));
            else n = metric(j, "memoryBytes");
            line->points = xrealloc(line->points, (line->count + 1) * sizeof *line->points);
            line->points[line->count++] = (StatsPoint){at.value, n};
        }
        if (line->count > 1) qsort(line->points, line->count, sizeof *line->points, by_time);
        size_t n = 0;
        for (size_t i = 0; i < line->count; i++) {
            StatsPoint p = line->points[i];
            if (n && line->points[n - 1].at_ms == p.at_ms) continue;
            line->points[n++] = p;
            if (!p.number.present) continue;
            line->valid_count++; line->latest = p.number;
            if (p.number.value > line->maximum) line->maximum = p.number.value;
        }
        line->count = n;
        if (n) { line->first_ms = line->points[0].at_ms; line->last_ms = line->points[n - 1].at_ms; }
    }
}
bool server_stats_parse(const Json *j, ServerStats *s) {
    memset(s, 0, sizeof *s);
    const Json *host = json_get(j, "host"), *p = json_get(j, "process"), *sessions = json_get(j, "sessions");
    if (!json_is_object(host) || !json_is_object(p) || !json_is_object(sessions)) return false;
    if (!json_is_array(json_get(j, "disks")) || !json_is_array(json_get(j, "history"))) return false;
    s->host = stats_host(host); s->cpu = stats_number(json_get(host, "cpuPercent"), false);
    s->free_memory = metric(host, "freeMemoryBytes"); s->uptime = stats_number(json_get(host, "uptimeSeconds"), false);
    for (int i = 0; i < 3; i++) s->load[i] = stats_number(json_at(json_get(host, "loadAverage"), (size_t)i), false);
    s->process = stats_figures(p); s->heap_used = metric(p, "heapUsedBytes"); s->heap_total = metric(p, "heapTotalBytes");
    s->process_uptime = stats_number(json_get(p, "uptimeSeconds"), false); s->node = xstrdup(json_str_nonempty(json_get(p, "nodeVersion")));
    s->sessions = stats_figures(sessions); s->open_sessions = metric(sessions, "open"); s->disk_bytes = metric(sessions, "diskBytes");
    const Json *disks = json_get(j, "disks");
    for (size_t i = 0; i < json_count(disks); i++) {
        const Json *d = json_at(disks, i); const char *label = json_str_nonempty(json_get(d, "label"));
        if (!json_is_object(d) || !label) continue;
        s->disks = xrealloc(s->disks, (s->disk_count + 1) * sizeof *s->disks);
        StatsDisk *disk = &s->disks[s->disk_count++];
        *disk = (StatsDisk){xstrdup(label), metric(d, "totalBytes"), metric(d, "freeBytes"), metric(d, "usedBytes")};
        // Explicit invalid usedBytes is unavailable, never replaced with a plausible number.
        bool has_used = false;
        for (size_t k = 0; k < json_count(d); k++) if (str_eq(json_key(d, k), "usedBytes")) has_used = true;
        if (!has_used) disk->used = stats_used(disk->total, disk->free_bytes);
    }
    history(s, json_get(j, "history")); return true;
}
void server_stats_free(ServerStats *s) {
    for (size_t i = 0; i < s->disk_count; i++) free(s->disks[i].label);
    for (int k = 0; k < 4; k++) stats_series_free(&s->lines[k]);
    free(s->disks); free(s->node); memset(s, 0, sizeof *s);
}
static int busiest(const void *a, const void *b) {
    const StatsSession *x = a, *y = b;
    double xc = x->figures.cpu.present ? x->figures.cpu.value : -1, yc = y->figures.cpu.present ? y->figures.cpu.value : -1;
    if (xc != yc) return xc > yc ? -1 : 1;
    double xm = x->figures.memory.present ? x->figures.memory.value : -1, ym = y->figures.memory.present ? y->figures.memory.value : -1;
    if (xm != ym) return xm > ym ? -1 : 1;
    return strcmp(x->id, y->id);
}
bool open_stats_parse(const Json *j, const Session *known, size_t count, OpenStats *s) {
    memset(s, 0, sizeof *s);
    const Json *host = json_get(j, "host"), *items = json_get(j, "sessions");
    if (!json_is_object(host) || !json_is_array(items)) return false;
    s->host = stats_host(host);
    for (size_t i = 0; i < json_count(items); i++) {
        const Json *raw = json_at(items, i); const char *id = json_str_nonempty(json_get(raw, "sessionId"));
        if (!id) continue;
        bool duplicate = false;
        for (size_t k = 0; k < s->count; k++) if (str_eq(s->items[k].id, id)) duplicate = true;
        if (duplicate) continue;
        s->items = xrealloc(s->items, (s->count + 1) * sizeof *s->items);
        StatsSession *row = &s->items[s->count++];
        *row = (StatsSession){.id = xstrdup(id), .figures = stats_figures(raw), .at_ms = stats_timestamp(json_get(raw, "at"))};
        row->disk_bytes = metric(json_get(raw, "disk"), "bytes"); row->disk_at_ms = stats_timestamp(json_get(json_get(raw, "disk"), "measuredAt"));
        for (size_t k = 0; k < count; k++) {
            if (!str_eq(session_id(&known[k]), id)) continue;
            session_copy(&row->record, &known[k]); break;
        }
        if (!row->record.raw) {
            row->record.raw = json_object(); json_set_str(row->record.raw, "id", id); json_set_str(row->record.raw, "title", id);
        }
        if (row->at_ms.value > s->at_ms.value) s->at_ms = row->at_ms;
    }
    if (s->count > 1) qsort(s->items, s->count, sizeof *s->items, busiest);
    return true;
}
void open_stats_free(OpenStats *s) {
    for (size_t i = 0; i < s->count; i++) { free(s->items[i].id); session_free(&s->items[i].record); }
    free(s->items); memset(s, 0, sizeof *s);
}
StatsSeries stats_series_copy(const StatsSeries *s) {
    StatsSeries copy = *s; copy.points = NULL;
    if (s->count) { copy.points = xmalloc(s->count * sizeof *copy.points); memcpy(copy.points, s->points, s->count * sizeof *copy.points); }
    return copy;
}
