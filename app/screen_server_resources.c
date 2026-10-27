#include "screen_resources_internal.h"
#include "stat_components.h"

static bool channel(ResourcesScreen *s, Doc *doc, int w, StatNumber at) {
    if (!resources_current(s)) {
        doc_notice(doc, 0, w, "This connection or token does not support these figures."); return false;
    }
    resources_discard_revoked(s);
    if (s->error) doc_notice(doc, 0, w, s->error);
    double until = resources_retry_until(s);
    if (until > (double)time(NULL)) {
        char *time = stats_time((StatNumber){until * 1000, true}); char *text = xstrfmt("Reads paused until %s", time);
        doc_text(doc, 0, w, text, FONT_CAPTION, theme.muted, DT_WORDBREAK); free(time); free(text);
    }
    if (!s->loaded) {
        if (!s->error && until <= (double)time(NULL)) doc_loading(doc, 0, w, "Loading resources…");
        return false;
    }
    char *read = stats_time((StatNumber){s->received_at * 1000, true}), *age = stats_age(at, (double)time(NULL));
    char *text = xstrfmt("%s · last read %s · %s", s->fresh ? "Last successful figures" : "Figures may be stale", read, age);
    doc_text(doc, 0, w, text, FONT_CAPTION, theme.muted, DT_WORDBREAK); free(text); free(read); free(age);
    return true;
}
static void session_rows(ResourcesScreen *s, Doc *doc, int w) {
    ResourcesScreen *list = s->list;
    doc_section(doc, 0, w, "Open sessions · busiest first");
    if (!channel(list, doc, w, list->sessions.at_ms)) return;
    if (!list->sessions.count) doc_text(doc, 0, w, "No session is open.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    s->action_first = s->action_next; s->action_count = list->sessions.count;
    s->action_next += (intptr_t)s->action_count;
    for (size_t i = 0; i < list->sessions.count; i++) {
        const StatsSession *row = &list->sessions.items[i];
        char *cpu = stats_percent(row->figures.cpu), *mem = stats_bytes(row->figures.memory);
        char *count = stats_processes(row->figures.processes, true), *disk = stats_bytes(row->disk_bytes);
        char *age = stats_age(row->at_ms, (double)time(NULL)), *disk_at = stats_time(row->disk_at_ms);
        char *line = xstrfmt("%s of one core · %s · %s · %s on disk (measured %s) · %s", cpu, mem, count, disk, disk_at, age);
        int box = doc_box_begin(doc, 0, w, px(12), theme.raise, theme.line, px(12));
        doc_button(doc, px(12), w - px(24), session_display_title(&row->record), BUTTON_BORDERED, RESOURCES_OPEN, s->action_first + (intptr_t)i, store_supports("session_stats"));
        doc_text(doc, px(12), w - px(24), line, FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
        doc_stat_gauge(doc, px(12), w - px(24), stats_cpu_share(&row->figures, &list->sessions.host));
        doc_box_end(doc, box, px(12)); doc_space(doc, px(8));
        free(cpu); free(mem); free(count); free(disk); free(line); free(age); free(disk_at);
    }
}
void server_resources_layout(Screen *base, Doc *doc) {
    ResourcesScreen *s = (ResourcesScreen *)base; int w = doc->width;
    s->action_count = 0; doc_space(doc, px(12));
    if (!resources_current(s)) { doc_notice(doc, 0, w, "Reopen Server on a connection with resource access."); return; }
    resources_discard_revoked(s);
    if (channel(s, doc, w, s->server.at_ms)) {
        StatTile tiles[6]; server_stats_tiles(&s->server, tiles); doc_stat_tiles(doc, 0, w, tiles, 6); stats_tiles_free(tiles, 6);
        doc_text(doc, 0, w, "Byte labels use powers of 1024. Process CPU is relative to one core; CPU gauges and chart lines use the whole machine.", FONT_CAPTION, theme.muted, DT_WORDBREAK);
        if (s->server.disk_count) doc_section(doc, 0, w, "Disks");
        for (size_t i = 0; i < s->server.disk_count; i++) {
            StatTile tile = server_stats_disk_tile(&s->server.disks[i]); doc_stat_tiles(doc, 0, w, &tile, 1); stats_tiles_free(&tile, 1);
        }
        doc_section(doc, 0, w, "Last hour");
        for (int k = 0; k < 4; k++) doc_stats_chart(doc, 0, w, stats_series_copy(&s->server.lines[k]));
    }
    session_rows(s, doc, w);
}
