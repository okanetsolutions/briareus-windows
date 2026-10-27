#include "stats.h"
#include "str.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static StatNumber number(const Json *j, bool integer) {
    double n;
    if (!json_num(j, &n) || !isfinite(n) || n < 0) return (StatNumber){0};
    if (integer && (floor(n) != n || n > 9007199254740991.0)) return (StatNumber){0};
    return (StatNumber){n, true};
}
static StatNumber timestamp(const Json *j) {
    StatNumber n = number(j, true);
    // JavaScript Date's supported range; formatting never narrows this to a time_t.
    if (n.value <= 0 || n.value > 8640000000000000.0) n = (StatNumber){0};
    return n;
}
static ProcessStats figures(const Json *j) {
    return (ProcessStats){number(json_get(j, "cpuPercent"), false), number(json_get(j, "memoryBytes"), true), number(json_get(j, "processes"), true)};
}
static int by_time(const void *a, const void *b) {
    double x = ((const StatsSample *)a)->at_ms, y = ((const StatsSample *)b)->at_ms;
    return (x > y) - (x < y);
}
bool session_stats_parse(const Json *answer, const char *session_id_, SessionStats *out) {
    memset(out, 0, sizeof *out);
    const Json *host = json_get(answer, "host"), *s = json_get(answer, "stats"), *history = json_get(answer, "history");
    if (!json_is_object(answer) || !json_is_object(host) || !json_is_array(history)) return false;
    // A missing stats key is malformed, rather than evidence that the session is closed.
    bool has_stats = false;
    for (size_t i = 0; i < json_count(answer); i++) if (str_eq(json_key(answer, i), "stats")) has_stats = true;
    if (!has_stats) return false;
    if (!json_is_null(s) && !json_is_object(s)) return false;
    if (json_is_object(s) && !str_eq(json_str_nonempty(json_get(s, "sessionId")), session_id_)) return false;
    out->host = (StatsHost){number(json_get(host, "cpus"), true), number(json_get(host, "memoryBytes"), true), number(json_get(host, "intervalMs"), true)};
    out->open = json_is_object(s);
    if (!out->open) return true;
    out->total = figures(s); out->at_ms = timestamp(json_get(s, "at"));
    const char *keys[] = {"agent", "serve", "browser"};
    for (int r = 0; r < STATS_ROLES; r++) {
        const Json *role = json_get(json_get(s, "roles"), keys[r]);
        out->roles[r] = json_is_object(role); out->role[r] = figures(role);
    }
    out->disk_bytes = number(json_get(json_get(s, "disk"), "bytes"), true);
    out->disk_at_ms = timestamp(json_get(json_get(s, "disk"), "measuredAt"));
    double latest = out->at_ms.value;
    for (size_t i = 0; i < json_count(history); i++) {
        StatNumber at = timestamp(json_get(json_at(history, i), "at"));
        if (at.present && at.value > latest) latest = at.value;
    }
    for (size_t i = 0; i < json_count(history); i++) {
        const Json *sample = json_at(history, i); StatNumber at = timestamp(json_get(sample, "at"));
        if (!at.present || at.value < latest - 3600000) continue;
        out->history = xrealloc(out->history, (out->history_count + 1) * sizeof *out->history);
        out->history[out->history_count++] = (StatsSample){at.value, figures(sample)};
    }
    if (out->history_count > 1) qsort(out->history, out->history_count, sizeof *out->history, by_time);
    // Duplicate timestamps cannot create a line or a zero-width chart interval.
    size_t n = 0;
    for (size_t i = 0; i < out->history_count; i++) {
        if (n && out->history[n - 1].at_ms == out->history[i].at_ms) continue;
        out->history[n++] = out->history[i];
    }
    out->history_count = n;
    return true;
}
void session_stats_free(SessionStats *s) { free(s->history); memset(s, 0, sizeof *s); }
const char *stats_role_label(int role) {
    static const char *const labels[] = {"Agent", "\xE2\x96\xB6 Run", "Shared browser"};
    return role >= 0 && role < STATS_ROLES ? labels[role] : "";
}
StatNumber stats_share(StatNumber part, StatNumber whole) {
    if (!part.present || !whole.present || whole.value <= 0) return (StatNumber){0};
    if (!isfinite(part.value) || !isfinite(whole.value) || part.value < 0) return (StatNumber){0};
    double share = part.value / whole.value;
    return (StatNumber){share > 1 ? 1 : share, true};
}
StatTone stats_tone(StatNumber fraction) {
    if (!fraction.present || !isfinite(fraction.value) || fraction.value < 0) return STAT_MUTED;
    if (fraction.value >= 0.9) return STAT_DANGER;
    if (fraction.value >= 0.75) return STAT_WARN;
    return STAT_OK;
}
static bool available(StatNumber n) { return n.present && isfinite(n.value) && n.value >= 0; }
char *stats_bytes(StatNumber n) {
    if (!available(n)) return xstrdup("\xE2\x80\x94");
    static const char *const units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    int unit = 0;
    while (n.value >= 1024 && unit < 5) { n.value /= 1024; unit++; }
    return xstrfmt(unit && n.value < 9.95 ? "%.1f %s" : "%.0f %s", n.value, units[unit]);
}
char *stats_percent(StatNumber n) {
    if (!available(n)) return xstrdup("\xE2\x80\x94");
    return xstrfmt(n.value > 0 && n.value < 0.95 ? "%.1f%%" : "%.0f%%", n.value);
}
char *stats_processes(StatNumber n, bool label) {
    if (!available(n)) return xstrdup(label ? "Processes unavailable" : "\xE2\x80\x94");
    return label ? xstrfmt("%.0f process%s", n.value, n.value == 1 ? "" : "es") : xstrfmt("%.0f", n.value);
}
char *stats_time(StatNumber ms) {
    if (!available(ms) || ms.value <= 0) return xstrdup("unavailable");
    double minutes = floor(ms.value / 60000);
    return xstrfmt("%02d:%02d UTC", (int)fmod(floor(minutes / 60), 24), (int)fmod(minutes, 60));
}
int stats_poll_ms(const StatsHost *host) {
    if (!host || !available(host->interval_ms) || host->interval_ms.value <= 0) return 5000;
    double ms = host->interval_ms.value;
    if (ms < 2000) return 2000;
    return ms > INT_MAX ? INT_MAX : (int)ms;
}
double stats_retry_deadline(double now, double previous, int failures, double retry_after) {
    int capped = failures < 1 ? 1 : failures > 6 ? 6 : failures;
    double delay = pow(2, capped);
    if (delay > 60) delay = 60;
    if (isfinite(retry_after) && retry_after > delay) delay = retry_after;
    double until = now + ceil(delay);
    return previous > until ? previous : until;
}
