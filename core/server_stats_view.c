#include "server_stats.h"
#include "stats_internal.h"
#include "str.h"
#include <math.h>
#include <stdlib.h>

char *stats_uptime(StatNumber n) {
    if (!n.present || !isfinite(n.value) || n.value < 0) return xstrdup("—");
    double seconds = floor(n.value), days = floor(seconds / 86400);
    int hours = (int)fmod(floor(seconds / 3600), 24), minutes = (int)fmod(floor(seconds / 60), 60);
    if (days > 0) return xstrfmt("%.0fd %dh", days, hours);
    if (hours > 0) return xstrfmt("%dh %dm", hours, minutes);
    if (minutes > 0) return xstrfmt("%dm", minutes);
    return xstrfmt("%.0fs", seconds);
}
char *stats_age(StatNumber at_ms, double now) {
    if (!at_ms.present || !isfinite(now) || at_ms.value > now * 1000) return xstrdup("sample age unavailable");
    char *age = stats_uptime((StatNumber){now - at_ms.value / 1000, true});
    char *text = xstrfmt("sample age %s", age); free(age); return text;
}
static char *decimal(StatNumber n) { return n.present ? xstrfmt("%.2f", n.value) : xstrdup("—"); }
void server_stats_tiles(const ServerStats *s, StatTile out[6]) {
    StatNumber used = stats_used(s->host.memory, s->free_memory);
    char *cores = stats_processes(s->host.cpus, false), *memory = stats_bytes(s->host.memory), *free_mem = stats_bytes(s->free_memory);
    char *load[3]; for (int i = 0; i < 3; i++) load[i] = decimal(s->load[i]);
    out[0] = (StatTile){"Machine CPU", stats_percent(s->cpu), xstrfmt("of all %s cores", cores), stats_share(s->cpu, (StatNumber){100, true})};
    out[1] = (StatTile){"Machine memory", stats_bytes(used), xstrfmt("of %s · %s free", memory, free_mem), stats_share(used, s->host.memory)};
    out[2] = (StatTile){"Load", decimal(s->load[0]), xstrfmt("%s · %s · %s (1 / 5 / 15 min)", load[0], load[1], load[2]), stats_share(s->load[0], s->host.cpus)};
    out[3] = (StatTile){"Machine up", stats_uptime(s->uptime), xstrdup("since its last start"), {0}};
    char *process_mem = stats_bytes(s->process.memory), *up = stats_uptime(s->process_uptime);
    out[4] = (StatTile){"Briareus server", stats_percent(s->process.cpu), xstrfmt("of one core · %s · up %s%s%s", process_mem, up, s->node ? " · Node " : "", s->node ? s->node : ""), stats_cpu_share(&s->process, &s->host)};
    char *cpu = stats_percent(s->sessions.cpu), *sessions_mem = stats_bytes(s->sessions.memory), *disk = stats_bytes(s->disk_bytes);
    out[5] = (StatTile){"Open sessions", stats_processes(s->open_sessions, false), xstrfmt("%s of one core · %s · %s on disk", cpu, sessions_mem, disk), stats_cpu_share(&s->sessions, &s->host)};
    free(cores); free(memory); free(free_mem); for (int i = 0; i < 3; i++) free(load[i]);
    free(process_mem); free(up); free(cpu); free(sessions_mem); free(disk);
}
StatTile server_stats_disk_tile(const StatsDisk *d) {
    const char *label = str_eq(d->label, "workspaces") ? "Workspaces" : str_eq(d->label, "files") ? "Files and recordings" : d->label;
    char *total = stats_bytes(d->total), *free_bytes = stats_bytes(d->free_bytes);
    StatTile tile = {label, stats_bytes(d->used), xstrfmt("of %s · %s free", total, free_bytes), stats_share(d->used, d->total)};
    free(total); free(free_bytes); return tile;
}
